#!/usr/bin/env python3
"""Compile the human-readable .slugui language into typed C++ SlugUI IR."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from svg_path import Bounds, SvgPathError, operation_bounds, parse_svg_path


class CompileError(Exception):
    def __init__(self, message: str, token: "Token | None" = None):
        super().__init__(message)
        self.message = message
        self.token = token


@dataclass(frozen=True)
class Token:
    kind: str
    value: str
    line: int
    column: int


class Lexer:
    SYMBOLS = set("{}[]():;=,$%")

    def __init__(self, source: str):
        self.source = source
        self.index = 0
        self.line = 1
        self.column = 1

    def _peek(self, offset: int = 0) -> str:
        position = self.index + offset
        return self.source[position] if position < len(self.source) else ""

    def _take(self) -> str:
        value = self._peek()
        if not value:
            return ""
        self.index += 1
        if value == "\n":
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        return value

    def _skip_space_and_comments(self) -> None:
        while True:
            while self._peek() and self._peek().isspace():
                self._take()
            if self._peek() == "/" and self._peek(1) == "/":
                while self._peek() and self._take() != "\n":
                    pass
                continue
            if self._peek() == "/" and self._peek(1) == "*":
                start = Token("", "", self.line, self.column)
                self._take()
                self._take()
                while self._peek() and not (self._peek() == "*" and self._peek(1) == "/"):
                    self._take()
                if not self._peek():
                    raise CompileError("unterminated block comment", start)
                self._take()
                self._take()
                continue
            return

    def tokens(self) -> list[Token]:
        result: list[Token] = []
        while True:
            self._skip_space_and_comments()
            line, column = self.line, self.column
            current = self._peek()
            if not current:
                result.append(Token("EOF", "", line, column))
                return result
            if current in self.SYMBOLS:
                result.append(Token(current, self._take(), line, column))
                continue
            if current == "#":
                self._take()
                digits = ""
                while self._peek() and self._peek().lower() in "0123456789abcdef":
                    digits += self._take()
                if len(digits) not in (6, 8):
                    raise CompileError("colors must use #RRGGBB or #RRGGBBAA", Token("COLOR", digits, line, column))
                result.append(Token("COLOR", digits.lower(), line, column))
                continue
            if current == '"':
                start = self.index
                self._take()
                escaped = False
                while self._peek():
                    value = self._take()
                    if value == '"' and not escaped:
                        break
                    if value == "\n" and not escaped:
                        raise CompileError("newline in string literal", Token("STRING", "", line, column))
                    escaped = value == "\\" and not escaped
                    if value != "\\":
                        escaped = False
                else:
                    raise CompileError("unterminated string literal", Token("STRING", "", line, column))
                raw = self.source[start:self.index]
                try:
                    decoded = json.loads(raw)
                except json.JSONDecodeError as error:
                    raise CompileError(f"invalid string escape: {error.msg}", Token("STRING", "", line, column)) from error
                result.append(Token("STRING", decoded, line, column))
                continue
            if current.isdigit() or (current in "+-" and self._peek(1).isdigit()):
                value = self._take()
                while self._peek().isdigit():
                    value += self._take()
                if self._peek() == ".":
                    value += self._take()
                    if not self._peek().isdigit():
                        raise CompileError("expected digits after decimal point", Token("NUMBER", value, line, column))
                    while self._peek().isdigit():
                        value += self._take()
                if self._peek().lower() == "e":
                    value += self._take()
                    if self._peek() in "+-":
                        value += self._take()
                    if not self._peek().isdigit():
                        raise CompileError("invalid number exponent", Token("NUMBER", value, line, column))
                    while self._peek().isdigit():
                        value += self._take()
                result.append(Token("NUMBER", value, line, column))
                continue
            if current.isalpha() or current == "_":
                value = self._take()
                while self._peek() and (self._peek().isalnum() or self._peek() in "_-"):
                    value += self._take()
                result.append(Token("IDENT", value, line, column))
                continue
            raise CompileError(f"unexpected character {current!r}", Token("", current, line, column))


@dataclass
class Value:
    kind: str
    value: object
    token: Token
    arguments: list["Value"] = field(default_factory=list)


@dataclass
class PropertyDecl:
    type_name: str
    name: str
    initial: Value
    token: Token


@dataclass
class Attribute:
    name: str
    value: Value
    token: Token


@dataclass
class ElementDecl:
    kind: str
    name: str
    attributes: list[Attribute]
    children: list["ElementDecl"]
    token: Token


@dataclass
class ComponentDecl:
    name: str
    properties: list[PropertyDecl]
    root: ElementDecl
    token: Token


class Parser:
    PROPERTY_TYPES = {
        "bool", "int", "float", "string", "color", "paint", "length", "radii",
        "smoothing", "border-widths",
    }

    def __init__(self, tokens: list[Token]):
        self.tokens = tokens
        self.index = 0

    def _peek(self, offset: int = 0) -> Token:
        return self.tokens[min(self.index + offset, len(self.tokens) - 1)]

    def _take(self) -> Token:
        value = self._peek()
        self.index += 1
        return value

    def _accept(self, kind: str, value: str | None = None) -> Token | None:
        token = self._peek()
        if token.kind == kind and (value is None or token.value == value):
            self.index += 1
            return token
        return None

    def _expect(self, kind: str, value: str | None = None, message: str | None = None) -> Token:
        token = self._accept(kind, value)
        if token is None:
            expected = value if value is not None else kind
            raise CompileError(message or f"expected {expected}", self._peek())
        return token

    def parse(self) -> ComponentDecl:
        component_token = self._expect("IDENT", "component", "file must start with 'component'")
        name = self._expect("IDENT", message="expected component name").value
        self._expect("{")
        properties: list[PropertyDecl] = []
        roots: list[ElementDecl] = []
        while not self._accept("}"):
            if self._peek().kind == "EOF":
                raise CompileError("unterminated component", component_token)
            if self._peek().kind == "IDENT" and self._peek().value == "property":
                properties.append(self._parse_property())
            else:
                roots.append(self._parse_element())
        self._expect("EOF", message="only one component is allowed per file")
        if len(roots) != 1:
            raise CompileError("component must contain exactly one root element", component_token)
        names: set[str] = set()
        for prop in properties:
            if prop.name in names:
                raise CompileError(f"duplicate property '{prop.name}'", prop.token)
            names.add(prop.name)
        return ComponentDecl(name, properties, roots[0], component_token)

    def _parse_property(self) -> PropertyDecl:
        token = self._expect("IDENT", "property")
        type_token = self._expect("IDENT", message="expected property type")
        if type_token.value not in self.PROPERTY_TYPES:
            raise CompileError(f"unknown property type '{type_token.value}'", type_token)
        name = self._expect("IDENT", message="expected property name").value
        self._expect("=")
        initial = self._parse_value()
        self._expect(";")
        return PropertyDecl(type_token.value, name, initial, token)

    def _parse_element(self) -> ElementDecl:
        kind = self._expect("IDENT", message="expected element kind")
        name = self._expect("IDENT", message="expected stable element name").value
        self._expect("{")
        attributes: list[Attribute] = []
        children: list[ElementDecl] = []
        attribute_names: set[str] = set()
        while not self._accept("}"):
            if self._peek().kind == "EOF":
                raise CompileError(f"unterminated element '{name}'", kind)
            if self._peek().kind != "IDENT":
                raise CompileError("expected attribute or child element", self._peek())
            if self._peek(1).kind == ":":
                attribute_token = self._take()
                self._take()
                if attribute_token.value in attribute_names:
                    raise CompileError(f"duplicate attribute '{attribute_token.value}'", attribute_token)
                attribute_names.add(attribute_token.value)
                attributes.append(Attribute(attribute_token.value, self._parse_value(), attribute_token))
                self._expect(";")
            else:
                children.append(self._parse_element())
        return ElementDecl(kind.value, name, attributes, children, kind)

    def _parse_value(self) -> Value:
        token = self._peek()
        if self._accept("$"):
            name = self._expect("IDENT", message="expected property name after '$'")
            return Value("reference", name.value, token)
        if value := self._accept("STRING"):
            return Value("string", value.value, value)
        if value := self._accept("COLOR"):
            return Value("color", value.value, value)
        if value := self._accept("NUMBER"):
            if self._peek().kind == "IDENT" and self._peek().value in ("px", "ppx"):
                unit = self._take().value
                return Value("length", (value.value, unit), value)
            if self._accept("%"):
                return Value("length", (value.value, "%"), value)
            return Value("number", value.value, value)
        if value := self._accept("IDENT"):
            if value.value in ("true", "false"):
                return Value("bool", value.value == "true", value)
            if self._accept("("):
                arguments: list[Value] = []
                if not self._accept(")"):
                    while True:
                        arguments.append(self._parse_value())
                        if self._accept(")"):
                            break
                        self._expect(",", message="expected ',' or ')' in function call")
                return Value("call", value.value, value, arguments)
            return Value("enum", value.value, value)
        if opening := self._accept("["):
            arguments: list[Value] = []
            if not self._accept("]"):
                while True:
                    arguments.append(self._parse_value())
                    if self._accept("]"):
                        break
                    self._expect(",", message="expected ',' or ']' in list")
            return Value("list", None, opening, arguments)
        raise CompileError("expected value", token)


def parse_source(source: str) -> ComponentDecl:
    return Parser(Lexer(source).tokens()).parse()


PROPERTY_CPP = {
    "bool": "bool",
    "int": "std::int64_t",
    "float": "float",
    "string": "std::string",
    "color": "slugvk::Color",
    "paint": "slugvk::Paint",
    "length": "slugvk::slugui::Length",
    "radii": "slugvk::CornerRadii",
    "smoothing": "slugvk::CornerSmoothing",
    "border-widths": "slugvk::BorderWidths",
}


def cpp_identifier(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9_]", "_", value)
    if not result or result[0].isdigit():
        result = "_" + result
    if result in {
        "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch",
        "char", "class", "const", "constexpr", "continue", "default", "delete", "do",
        "double", "else", "enum", "explicit", "export", "extern", "false", "float",
        "for", "friend", "goto", "if", "inline", "int", "long", "namespace", "new",
        "noexcept", "not", "nullptr", "operator", "or", "private", "protected",
        "public", "register", "reinterpret_cast", "return", "short", "signed",
        "sizeof", "static", "struct", "switch", "template", "this", "throw", "true",
        "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void",
        "volatile", "while", "xor",
    }:
        result += "_"
    return result


def cpp_string(value: str) -> str:
    escaped: list[str] = []
    for byte in value.encode("utf-8"):
        if byte == 0x22:
            escaped.append(r'\"')
        elif byte == 0x5C:
            escaped.append(r"\\")
        elif byte == 0x0A:
            escaped.append(r"\n")
        elif byte == 0x0D:
            escaped.append(r"\r")
        elif byte == 0x09:
            escaped.append(r"\t")
        elif 0x20 <= byte <= 0x7E:
            escaped.append(chr(byte))
        else:
            escaped.append(f"\\{byte:03o}")
    return '"' + "".join(escaped) + '"'


def cpp_float(raw: str) -> str:
    if "e" not in raw.lower() and "." not in raw:
        raw += ".0"
    return raw + "f"


def number_text(value: Value, *, integer: bool = False) -> str:
    if value.kind != "number":
        raise CompileError("expected number", value.token)
    raw = str(value.value)
    if integer:
        try:
            parsed = float(raw)
        except ValueError as error:
            raise CompileError("invalid integer", value.token) from error
        if not parsed.is_integer():
            raise CompileError("integer property requires an integer literal", value.token)
        return str(int(parsed))
    return cpp_float(raw)


def color_cpp(value: Value) -> str:
    if value.kind != "color":
        raise CompileError("expected #RRGGBB or #RRGGBBAA color", value.token)
    digits = str(value.value)
    if len(digits) == 6:
        return f"slugvk::Color::fromRgb8(0x{digits})"
    alpha = int(digits[6:8], 16) / 255.0
    return f"slugvk::Color::fromRgb8(0x{digits[:6]}, {cpp_float(f'{alpha:.8g}')})"


def scalar_from_value(value: Value, *, allow_percent: bool = False) -> str:
    if value.kind == "number":
        return number_text(value)
    if allow_percent and value.kind == "length" and value.value[1] == "%":
        return cpp_float(str(value.value[0]))
    raise CompileError("expected scalar", value.token)


def length_cpp(value: Value) -> str:
    if value.kind == "enum" and value.value == "auto":
        return "slugvk::slugui::Length::autoSize()"
    if value.kind == "number":
        return f"slugvk::slugui::Length::logical({number_text(value)})"
    if value.kind != "length":
        raise CompileError("expected length (px, ppx, %, or auto)", value.token)
    raw, unit = value.value
    scalar = Value("number", raw, value.token)
    method = {"px": "logical", "ppx": "physical", "%": "percent"}[unit]
    return f"slugvk::slugui::Length::{method}({number_text(scalar)})"


def numeric_list(value: Value, allowed_sizes: set[int], label: str,
                 allow_percent: bool = False) -> list[str]:
    values = value.arguments if value.kind == "list" else [value]
    if len(values) not in allowed_sizes:
        sizes = ", ".join(str(size) for size in sorted(allowed_sizes))
        raise CompileError(f"{label} expects {sizes} value(s)", value.token)
    return [scalar_from_value(item, allow_percent=allow_percent) for item in values]


def radii_cpp(value: Value) -> str:
    values = numeric_list(value, {1, 4}, "radius")
    if len(values) == 1:
        return f"slugvk::CornerRadii::all({values[0]})"
    return "slugvk::CornerRadii{" + ", ".join(values) + "}"


def smoothing_cpp(value: Value) -> str:
    values = numeric_list(value, {1, 4}, "smoothing", allow_percent=True)
    if len(values) == 1:
        return f"slugvk::CornerSmoothing::all({values[0]})"
    return "slugvk::CornerSmoothing{" + ", ".join(values) + "}"


def border_widths_cpp(value: Value) -> str:
    values = numeric_list(value, {4}, "stroke-width")
    return "slugvk::BorderWidths{" + ", ".join(values) + "}"


def paint_cpp(value: Value) -> str:
    if value.kind == "color":
        return f"slugvk::Paint::solid({color_cpp(value)})"
    if value.kind != "call":
        raise CompileError("expected color or paint function", value.token)
    name = str(value.value)
    args = value.arguments
    if name == "solid":
        if len(args) not in (1, 2):
            raise CompileError("solid(color[, opacity]) expects 1 or 2 arguments", value.token)
        result = f"slugvk::Paint::solid({color_cpp(args[0])}"
        if len(args) == 2:
            result += f", {scalar_from_value(args[1])}"
        return result + ")"
    kinds = {
        "linear": "Linear",
        "diamond": "Diamond",
        "radial": "Radial",
    }
    if name in kinds:
        if len(args) not in (2, 6, 7):
            raise CompileError(
                f"{name}(start, end[, ox, oy, tx, ty[, opacity]]) expects 2, 6, or 7 arguments",
                value.token,
            )
        result = (
            f"slugvk::Paint::gradient(slugvk::GradientKind::{kinds[name]}, "
            f"{color_cpp(args[0])}, {color_cpp(args[1])}"
        )
        if len(args) >= 6:
            result += (
                f", {{{scalar_from_value(args[2])}, {scalar_from_value(args[3])}}}, "
                f"{{{scalar_from_value(args[4])}, {scalar_from_value(args[5])}}}"
            )
        if len(args) == 7:
            result += f", {scalar_from_value(args[6])}"
        return result + ")"
    if name == "shader":
        if len(args) not in (2, 3, 7, 8):
            raise CompileError(
                "shader(start, end[, parameter[, ox, oy, tx, ty[, opacity]]]) has invalid arguments",
                value.token,
            )
        parameter = scalar_from_value(args[2]) if len(args) >= 3 else "0.0f"
        result = f"slugvk::Paint::shader({color_cpp(args[0])}, {color_cpp(args[1])}, {parameter}"
        if len(args) >= 7:
            result += (
                f", {{{scalar_from_value(args[3])}, {scalar_from_value(args[4])}}}, "
                f"{{{scalar_from_value(args[5])}, {scalar_from_value(args[6])}}}"
            )
        if len(args) == 8:
            result += f", {scalar_from_value(args[7])}"
        return result + ")"
    raise CompileError(f"unknown paint function '{name}'", value.token)


def literal_cpp(value: Value, expected: str) -> str:
    if expected == "bool":
        if value.kind != "bool":
            raise CompileError("expected boolean", value.token)
        return "true" if value.value else "false"
    if expected == "int":
        return number_text(value, integer=True)
    if expected == "float":
        return scalar_from_value(value)
    if expected == "string":
        if value.kind != "string":
            raise CompileError("expected string", value.token)
        return f"std::string{{{cpp_string(str(value.value))}}}"
    if expected == "color":
        return color_cpp(value)
    if expected == "paint":
        return paint_cpp(value)
    if expected == "length":
        return length_cpp(value)
    if expected == "radii":
        return radii_cpp(value)
    if expected == "smoothing":
        return smoothing_cpp(value)
    if expected == "border-widths":
        return border_widths_cpp(value)
    raise AssertionError(expected)


class Generator:
    KINDS = {"Absolute", "Row", "Column", "Stack", "Rectangle", "Shape", "Text"}
    LAYOUT_MEMBERS = {
        "x": ("x", "length"),
        "y": ("y", "length"),
        "width": ("width", "length"),
        "height": ("height", "length"),
        "min-width": ("minWidth", "length"),
        "min-height": ("minHeight", "length"),
        "max-width": ("maxWidth", "length"),
        "max-height": ("maxHeight", "length"),
        "preferred-width": ("preferredWidth", "length"),
        "preferred-height": ("preferredHeight", "length"),
        "grow": ("grow", "float"),
        "spacing": ("spacing", "length"),
    }
    PROPERTY_EXPECTED = {
        "bool": "bool",
        "int": "int",
        "float": "float",
        "string": "string",
        "color": "color",
        "paint": "paint",
        "length": "length",
        "radii": "radii",
        "smoothing": "smoothing",
        "border-widths": "border-widths",
    }

    def __init__(self, component: ComponentDecl, namespace: str,
                 class_name: str | None = None, legacy_figma_coordinates: bool = False):
        self.component = component
        self.namespace = namespace
        self.class_name = class_name or cpp_identifier(component.name)
        self.legacy_figma_coordinates = legacy_figma_coordinates
        self.properties = {prop.name: prop for prop in component.properties}
        self.property_members = {name: cpp_identifier(name) for name in self.properties}
        if len(set(self.property_members.values())) != len(self.property_members):
            raise CompileError("property names collide after C++ identifier normalization", component.token)
        self.callbacks: dict[str, str] = {}
        self.node_counter = 0
        self.asset_counter = 0

    def _value(self, value: Value, expected: str) -> str:
        if value.kind == "reference":
            name = str(value.value)
            declaration = self.properties.get(name)
            if declaration is None:
                raise CompileError(f"unknown property '{name}'", value.token)
            actual = self.PROPERTY_EXPECTED[declaration.type_name]
            if actual != expected:
                raise CompileError(
                    f"property '{name}' is {declaration.type_name}, expected {expected}",
                    value.token,
                )
            return f"result.{self.property_members[name]}"
        return literal_cpp(value, expected)

    @staticmethod
    def _enum(value: Value, allowed: dict[str, str], label: str) -> str:
        if value.kind not in ("enum", "string"):
            raise CompileError(f"expected {label}", value.token)
        key = str(value.value).lower()
        if key not in allowed:
            raise CompileError(f"unknown {label} '{value.value}'", value.token)
        return allowed[key]

    @staticmethod
    def _plain_name(value: Value, label: str) -> str:
        if value.kind not in ("enum", "string"):
            raise CompileError(f"expected {label} name", value.token)
        return str(value.value)

    def _insets(self, value: Value) -> str:
        values = value.arguments if value.kind == "list" else [value]
        if len(values) not in (1, 2, 4):
            raise CompileError("padding expects 1, 2, or 4 lengths", value.token)
        resolved = [self._value(item, "length") for item in values]
        if len(resolved) == 1:
            return f"slugvk::slugui::Insets::all({resolved[0]})"
        if len(resolved) == 2:
            return f"slugvk::slugui::Insets::symmetric({resolved[1]}, {resolved[0]})"
        return "slugvk::slugui::Insets{" + ", ".join(resolved) + "}"

    def _line(self, lines: list[str], indent: int, text: str) -> None:
        lines.append("  " * indent + text)

    @staticmethod
    def _path_strings(value: Value, label: str) -> list[str]:
        values = value.arguments if value.kind == "list" else [value]
        if not values or any(item.kind != "string" for item in values):
            raise CompileError(f"{label} expects a path string or non-empty string list", value.token)
        return [str(item.value) for item in values]

    def _path_asset(self, attribute: Attribute, lines: list[str],
                    indent: int) -> tuple[str, Bounds]:
        index = self.asset_counter
        self.asset_counter += 1
        asset = f"asset_{index}"
        path = f"asset_path_{index}"
        self._line(lines, indent, f"slugvk::ShapeId {asset} = 0;")
        self._line(lines, indent, "if (atlas != nullptr) {")
        self._line(lines, indent + 1, f"slugvk::Path {path};")
        all_operations = []
        for data in self._path_strings(attribute.value, attribute.name):
            try:
                operations = parse_svg_path(data)
            except SvgPathError as error:
                raise CompileError(f"invalid SVG path data: {error}", attribute.token) from error
            all_operations.extend(operations)
            for operation in operations:
                # SVG/Figma paths use a y-down coordinate system. Slug's atlas is y-up; negating
                # every authored y coordinate here preserves the source orientation without adding
                # runtime transforms or per-instance state.
                values = list(operation.values)
                for coordinate in range(1, len(values), 2):
                    values[coordinate] = -values[coordinate]
                arguments = ", ".join(
                    cpp_float(f"{value:.9g}") for value in values
                )
                call = f"{path}.{operation.name}({arguments});" if arguments else (
                    f"{path}.{operation.name}();"
                )
                self._line(lines, indent + 1, call)
        bounds = operation_bounds(all_operations)
        if bounds is None or bounds.width <= 0.0 or bounds.height <= 0.0:
            raise CompileError("SVG path data has no two-dimensional drawable bounds",
                               attribute.token)
        self._line(lines, indent + 1, f"{asset} = atlas->addPath({path});")
        self._line(lines, indent, "}")
        return asset, bounds

    @staticmethod
    def _fixed_scalar(element: ElementDecl, name: str) -> float | None:
        attribute = next(
            (candidate for candidate in element.attributes if candidate.name == name), None,
        )
        if attribute is None:
            return None
        value = attribute.value
        raw: object
        if value.kind == "number":
            raw = value.value
        elif value.kind == "length" and value.value[1] in ("px", "ppx"):
            raw = value.value[0]
        else:
            return None
        try:
            extent = float(raw)
        except (TypeError, ValueError):
            return None
        return extent if math.isfinite(extent) else None

    @classmethod
    def _fixed_extent(cls, element: ElementDecl, name: str,
                      allow_zero: bool = False) -> float | None:
        extent = cls._fixed_scalar(element, name)
        if extent is None or extent < 0.0 or (extent == 0.0 and not allow_zero):
            return None
        return extent

    @staticmethod
    def _is_figma_element(element: ElementDecl) -> bool:
        provider = next(
            (candidate.value for candidate in element.attributes
             if candidate.name == "source-provider"), None)
        return (provider is not None and provider.kind == "string" and
                provider.value == "figma")

    @staticmethod
    def _placement_values(attribute: Attribute) -> list[float]:
        values = attribute.value.arguments if attribute.value.kind == "list" else []
        if len(values) != 4:
            raise CompileError(f"{attribute.name} expects [x, y, width, height]", attribute.token)
        parsed: list[float] = []
        for value in values:
            if value.kind == "number":
                raw = value.value
            elif value.kind == "length" and value.value[1] in ("px", "ppx"):
                raw = value.value[0]
            else:
                raise CompileError(f"{attribute.name} values must be px or numbers", value.token)
            try:
                number = float(raw)
            except (TypeError, ValueError) as error:
                raise CompileError(f"invalid {attribute.name} value", value.token) from error
            if not math.isfinite(number):
                raise CompileError(f"invalid {attribute.name} value", value.token)
            parsed.append(number)
        if parsed[2] <= 0.0 or parsed[3] <= 0.0:
            raise CompileError(f"{attribute.name} width and height must be positive", attribute.token)
        return parsed

    def _recovered_figma_owner(self, element: ElementDecl) -> tuple[float, float, float, float] | None:
        if not self._is_figma_element(element):
            return None
        width = self._fixed_extent(element, "width", allow_zero=True)
        height = self._fixed_extent(element, "height", allow_zero=True)
        if width is None or height is None or (width > 0.0 and height > 0.0):
            return None
        placements = [
            self._placement_values(attribute) for attribute in element.attributes
            if attribute.name in ("path-placement", "stroke-placement")
        ]
        if not placements:
            return None
        left = min(value[0] for value in placements)
        top = min(value[1] for value in placements)
        right = max(value[0] + value[2] for value in placements)
        bottom = max(value[1] + value[3] for value in placements)
        return left, top, right - left, bottom - top

    def _path_placement(self, element: ElementDecl, bounds: Bounds) -> str | None:
        width = self._fixed_extent(element, "width")
        height = self._fixed_extent(element, "height")
        if width is None or height is None:
            return None
        values = (
            bounds.minimum_x / width,
            bounds.minimum_y / height,
            bounds.width / width,
            bounds.height / height,
        )
        return "slugvk::Rect{" + ", ".join(
            cpp_float(f"{value:.9g}") for value in values
        ) + "}"

    def _explicit_path_placement(self, element: ElementDecl,
                                 attribute: Attribute | None) -> str | None:
        if attribute is None:
            return None
        width = self._fixed_extent(element, "width", allow_zero=True)
        height = self._fixed_extent(element, "height", allow_zero=True)
        if width is None or height is None:
            raise CompileError(f"{attribute.name} requires fixed width and height", attribute.token)
        parsed = self._placement_values(attribute)
        recovered_owner = self._recovered_figma_owner(element)
        if recovered_owner is not None:
            parsed[0] -= recovered_owner[0]
            parsed[1] -= recovered_owner[1]
            width = recovered_owner[2]
            height = recovered_owner[3]
        if width <= 0.0 or height <= 0.0:
            raise CompileError(f"{attribute.name} requires positive fixed width and height",
                               attribute.token)
        disjoint = (parsed[0] + parsed[2] < 0 or parsed[0] > width or
                    parsed[1] + parsed[3] < 0 or parsed[1] > height)
        if (self._is_figma_element(element) and recovered_owner is None and
                (self.legacy_figma_coordinates or disjoint)):
            parsed[0] = (width - parsed[2]) * 0.5
            parsed[1] = (height - parsed[3]) * 0.5
        normalized = (parsed[0] / width, parsed[1] / height,
                      parsed[2] / width, parsed[3] / height)
        return "slugvk::Rect{" + ", ".join(
            cpp_float(f"{value:.9g}") for value in normalized
        ) + "}"

    def _common_attribute(self, variable: str, attribute: Attribute,
                          lines: list[str], indent: int) -> bool:
        name, value = attribute.name, attribute.value
        if name in self.LAYOUT_MEMBERS:
            member, expected = self.LAYOUT_MEMBERS[name]
            self._line(lines, indent,
                       f"{variable}.layout.{member} = {self._value(value, expected)};")
            return True
        padding_members = {
            "padding-left": "left",
            "padding-top": "top",
            "padding-right": "right",
            "padding-bottom": "bottom",
        }
        if name == "padding":
            self._line(lines, indent, f"{variable}.layout.padding = {self._insets(value)};")
            return True
        if name in padding_members:
            self._line(
                lines, indent,
                f"{variable}.layout.padding.{padding_members[name]} = {self._value(value, 'length')};",
            )
            return True
        if name == "layout":
            enum_value = self._enum(value, {
                "absolute": "Absolute", "row": "Row", "column": "Column", "stack": "Stack",
            }, "layout")
            self._line(lines, indent,
                       f"{variable}.layout.kind = slugvk::slugui::LayoutKind::{enum_value};")
            return True
        if name in ("cross-align", "align-self"):
            enum_value = self._enum(value, {
                "start": "Start", "center": "Center", "end": "End", "stretch": "Stretch",
            }, "alignment")
            member = "crossAlignment" if name == "cross-align" else "alignSelf"
            self._line(lines, indent,
                       f"{variable}.layout.{member} = slugvk::slugui::Alignment::{enum_value};")
            return True
        if name == "justify":
            enum_value = self._enum(value, {
                "start": "Start", "center": "Center", "end": "End",
                "space-between": "SpaceBetween", "spacebetween": "SpaceBetween",
            }, "justification")
            self._line(lines, indent,
                       f"{variable}.layout.mainAlignment = slugvk::slugui::Justify::{enum_value};")
            return True
        if name == "visible":
            self._line(lines, indent, f"{variable}.visible = {self._value(value, 'bool')};")
            return True
        if name in ("clip", "overlay", "interactive"):
            resolved = self._value(value, "bool")
            member = {"clip": "clipsChildren", "overlay": "overlay",
                      "interactive": "interaction.interactive"}[name]
            self._line(lines, indent, f"{variable}.{member} = {resolved};")
            return True
        if name == "enabled":
            self._line(lines, indent,
                       f"{variable}.interaction.enabled = {self._value(value, 'bool')};")
            return True
        if name == "callback":
            callback = self._plain_name(value, "callback")
            member = "callback_" + cpp_identifier(callback)
            previous = next((key for key, item in self.callbacks.items() if item == member), None)
            if previous is not None and previous != callback:
                raise CompileError("callback names collide after C++ normalization", value.token)
            self.callbacks[callback] = member
            self._line(lines, indent, f"{variable}.interaction.interactive = true;")
            self._line(lines, indent, f"{variable}.interaction.callback = {member};")
            return True
        source_members = {
            "source-provider": "provider",
            "source-document": "documentId",
            "source-node": "nodeId",
            "source-name": "nodeName",
        }
        if name in source_members:
            if value.kind != "string":
                raise CompileError(f"{name} expects a string", value.token)
            self._line(lines, indent,
                       f"{variable}.source.{source_members[name]} = {cpp_string(str(value.value))};")
            return True
        if name == "fidelity":
            enum_value = self._enum(value, {
                "native": "Native", "approximated": "Approximated",
                "baked-vector": "BakedVector", "bakedvector": "BakedVector",
                "baked-raster": "BakedRaster", "bakedraster": "BakedRaster",
                "unsupported": "Unsupported",
            }, "fidelity")
            self._line(lines, indent,
                       f"{variable}.source.fidelity = slugvk::slugui::ImportFidelity::{enum_value};")
            return True
        return False

    def _emit_text_runs(self, element: ElementDecl, variable: str,
                        lines: list[str], indent: int) -> None:
        attribute = next(
            (candidate for candidate in element.attributes if candidate.name == "text-runs"), None)
        if attribute is None:
            return
        values = attribute.value.arguments if attribute.value.kind == "list" else []
        if not values:
            raise CompileError("text-runs expects a non-empty list", attribute.token)
        text_attribute = next(
            (candidate for candidate in element.attributes if candidate.name == "text"), None)
        if text_attribute is None or text_attribute.value.kind != "string":
            raise CompileError("text-runs requires literal text", attribute.token)
        concatenated = ""
        visual = f"std::get<slugvk::slugui::TextVisual>({variable}.visual)"
        for index, value in enumerate(values):
            if value.kind != "call" or value.value != "text-run" or len(value.arguments) != 10:
                raise CompileError(
                    "each text-runs item must be text-run(text, font, size, weight, italic, "
                    "underline, strikethrough, letter-spacing, line-height, paint)",
                    value.token)
            (text, font, size, weight, italic, underline, strikethrough,
             letter_spacing, line_height, paint) = value.arguments
            if text.kind != "string" or font.kind != "string":
                raise CompileError("text-run text and font must be strings", value.token)
            if weight.kind != "number":
                raise CompileError("text-run weight must be a number", weight.token)
            parsed_weight = float(str(weight.value))
            if not math.isfinite(parsed_weight) or parsed_weight < 1 or parsed_weight > 1000:
                raise CompileError("text-run weight must be between 1 and 1000", weight.token)
            concatenated += str(text.value)
            style = f"text_run_style_{variable}_{index}"
            self._line(lines, indent, "{")
            self._line(lines, indent + 1, f"auto {style} = {visual}.style;")
            self._line(lines, indent + 1,
                       f"{style}.fontName = {cpp_string(str(font.value))};")
            self._line(lines, indent + 1,
                       f"{style}.size = {literal_cpp(size, 'float')};")
            self._line(lines, indent + 1,
                       f"{style}.weight = {int(round(parsed_weight))};")
            self._line(lines, indent + 1,
                       f"{style}.italic = {literal_cpp(italic, 'bool')};")
            self._line(lines, indent + 1,
                       f"{style}.underline = {literal_cpp(underline, 'bool')};")
            self._line(lines, indent + 1,
                       f"{style}.strikethrough = {literal_cpp(strikethrough, 'bool')};")
            self._line(lines, indent + 1,
                       f"{style}.letterSpacing = {literal_cpp(letter_spacing, 'float')};")
            self._line(lines, indent + 1,
                       f"{style}.lineHeight = {literal_cpp(line_height, 'float')};")
            self._line(lines, indent + 1,
                       f"{style}.paint = {paint_cpp(paint)};")
            self._line(
                lines, indent + 1,
                f"{visual}.runs.push_back(slugvk::TextRun{{"
                f"{cpp_string(str(text.value))}, std::move({style})}});")
            self._line(lines, indent, "}")
        if concatenated != str(text_attribute.value.value):
            raise CompileError("text-runs characters must exactly match text", attribute.token)

    def _visual_attribute(self, element: ElementDecl, variable: str, attribute: Attribute,
                          lines: list[str], indent: int) -> bool:
        name, value = attribute.name, attribute.value
        visual = f"std::get<slugvk::slugui::{element.kind if element.kind != 'Rectangle' else 'RoundedRectangle'}Visual>({variable}.visual)"
        if name in ("fill", "fill-hovered", "fill-pressed", "fill-disabled"):
            if element.kind not in ("Rectangle", "Shape", "Text"):
                raise CompileError(f"{name} requires a visual element", attribute.token)
            state = {
                "fill": "normal", "fill-hovered": "hovered",
                "fill-pressed": "pressed", "fill-disabled": "disabled",
            }[name]
            resolved = self._value(value, "paint")
            if state == "normal":
                self._line(lines, indent, f"{visual}.paint.normal = {resolved};")
            else:
                self._line(
                    lines, indent,
                    f"{visual}.paint.{state} = slugvk::slugui::ValueSource<slugvk::Paint>{{{resolved}}};",
                )
            return True
        if name in ("stroke", "stroke-hovered", "stroke-pressed", "stroke-disabled"):
            if element.kind not in ("Rectangle", "Shape"):
                raise CompileError(f"{name} requires Rectangle or Shape", attribute.token)
            state = {
                "stroke": "normal", "stroke-hovered": "hovered",
                "stroke-pressed": "pressed", "stroke-disabled": "disabled",
            }[name]
            resolved = self._value(value, "paint")
            target = f"{visual}.stroke.paint" if element.kind == "Rectangle" else (
                f"{visual}.strokePaint"
            )
            if state == "normal":
                self._line(lines, indent, f"{target}.normal = {resolved};")
            else:
                self._line(
                    lines, indent,
                    f"{target}.{state} = slugvk::slugui::ValueSource<slugvk::Paint>{{{resolved}}};",
                )
            return True
        if name == "stroke-width":
            if element.kind == "Rectangle":
                border_width_reference = value.kind == "reference" and (
                    self.properties.get(str(value.value)) is not None and
                    self.properties[str(value.value)].type_name == "border-widths"
                )
                if value.kind == "list" or border_width_reference:
                    self._line(lines, indent,
                               f"{visual}.stroke.individualWidths = "
                               f"slugvk::slugui::ValueSource<slugvk::BorderWidths>{{"
                               f"{self._value(value, 'border-widths')}}};")
                else:
                    self._line(lines, indent,
                               f"{visual}.stroke.width = {self._value(value, 'float')};")
            elif element.kind == "Shape":
                if value.kind == "list":
                    raise CompileError("Shape stroke-width must be uniform", attribute.token)
                self._value(value, 'float')
            else:
                raise CompileError("stroke-width requires Rectangle or Shape", attribute.token)
            return True
        if name == "stroke-align":
            if element.kind not in ("Rectangle", "Shape"):
                raise CompileError("stroke-align requires Rectangle or Shape", attribute.token)
            enum_value = self._enum(value, {
                "inside": "Inside", "center": "Center", "outside": "Outside",
            }, "stroke alignment")
            if element.kind == "Rectangle":
                self._line(lines, indent,
                           f"{visual}.stroke.align = slugvk::StrokeAlign::{enum_value};")
            return True
        if name in ("path-data", "stroke-path-data"):
            if element.kind != "Shape":
                raise CompileError(f"{name} requires Shape", attribute.token)
            self._path_strings(value, name)
            return True
        if name in ("path-placement", "stroke-placement"):
            if element.kind != "Shape":
                raise CompileError(f"{name} requires Shape", attribute.token)
            return True
        if name == "radius":
            if element.kind != "Rectangle":
                raise CompileError("radius requires Rectangle", attribute.token)
            self._line(lines, indent, f"{visual}.radii = {self._value(value, 'radii')};")
            return True
        if name == "smoothing":
            if element.kind != "Rectangle":
                raise CompileError("smoothing requires Rectangle", attribute.token)
            self._line(lines, indent,
                       f"{visual}.smoothing = {self._value(value, 'smoothing')};")
            return True
        if name == "shape":
            if element.kind != "Shape":
                raise CompileError("shape id requires Shape", attribute.token)
            self._line(lines, indent, f"{visual}.shape = {literal_cpp(value, 'int')};")
            return True
        if name == "text":
            if element.kind != "Text":
                raise CompileError("text requires Text", attribute.token)
            self._line(lines, indent, f"{visual}.text = {self._value(value, 'string')};")
            return True
        if name == "text-runs":
            if element.kind != "Text":
                raise CompileError("text-runs requires Text", attribute.token)
            return True
        if element.kind != "Text":
            return False
        style = f"{visual}.style"
        if name == "font":
            if value.kind != "string":
                raise CompileError("font expects a string", value.token)
            self._line(lines, indent, f"{style}.fontName = {cpp_string(str(value.value))};")
            return True
        if name == "font-weight":
            if value.kind != "number":
                raise CompileError("font-weight expects a number", value.token)
            weight = float(str(value.value))
            if not math.isfinite(weight) or weight < 1 or weight > 1000:
                raise CompileError("font-weight must be between 1 and 1000", value.token)
            self._line(lines, indent, f"{style}.weight = {int(round(weight))};")
            return True
        float_fields = {
            "font-size": "size", "line-height": "lineHeight",
            "letter-spacing": "letterSpacing", "indent": "indent",
        }
        if name in float_fields:
            self._line(lines, indent,
                       f"{style}.{float_fields[name]} = {literal_cpp(value, 'float')};")
            return True
        bool_fields = {
            "bold": "bold", "italic": "italic", "underline": "underline",
            "strikethrough": "strikethrough",
        }
        if name in bool_fields:
            self._line(lines, indent,
                       f"{style}.{bool_fields[name]} = {literal_cpp(value, 'bool')};")
            return True
        if name == "text-align":
            enum_value = self._enum(value, {
                "left": "Left", "center": "Center", "right": "Right", "justify": "Justify",
                "justified": "Justify",
            }, "text alignment")
            self._line(lines, indent,
                       f"{style}.align = slugvk::HorizontalAlign::{enum_value};")
            return True
        if name == "text-align-vertical":
            enum_value = self._enum(value, {
                "top": "Top", "center": "Center", "bottom": "Bottom",
            }, "vertical text alignment")
            self._line(lines, indent,
                       f"{style}.verticalAlign = slugvk::VerticalAlign::{enum_value};")
            return True
        if name == "list-marker":
            enum_value = self._enum(value, {
                "none": "None", "bullet": "Bullet", "numbered": "Numbered",
            }, "list marker")
            self._line(lines, indent,
                       f"{style}.listMarker = slugvk::ListMarker::{enum_value};")
            return True
        return False

    def _node(self, element: ElementDecl, path: str, lines: list[str], indent: int) -> str:
        if element.kind not in self.KINDS:
            raise CompileError(f"unknown element kind '{element.kind}'", element.token)
        variable = f"node_{self.node_counter}"
        self.node_counter += 1
        stable_id = f"{self.component.name}/"
        if path:
            stable_id += path + "/"
        stable_id += element.name
        path_attribute = next(
            (attribute for attribute in element.attributes if attribute.name == "path-data"), None)
        stroke_path_attribute = next(
            (attribute for attribute in element.attributes
              if attribute.name == "stroke-path-data"), None)
        fill_placement_attribute = next(
            (attribute for attribute in element.attributes
             if attribute.name == "path-placement"), None)
        stroke_placement_attribute = next(
            (attribute for attribute in element.attributes
             if attribute.name == "stroke-placement"), None)
        shape_attribute = next(
            (attribute for attribute in element.attributes if attribute.name == "shape"), None)
        if element.kind != "Shape" and (path_attribute or stroke_path_attribute):
            offending = path_attribute or stroke_path_attribute
            raise CompileError(f"{offending.name} requires Shape", offending.token)
        if path_attribute and shape_attribute:
            raise CompileError("Shape cannot specify both shape and path-data", shape_attribute.token)
        fill_asset, fill_bounds = (
            self._path_asset(path_attribute, lines, indent)
            if path_attribute else ("0", None)
        )
        stroke_asset, stroke_bounds = (
            self._path_asset(stroke_path_attribute, lines, indent)
            if stroke_path_attribute else ("0", None)
        )
        arguments = f"slugvk::hashId({cpp_string(stable_id)}), {cpp_string(element.name)}"
        factories = {
            "Absolute": f"slugvk::slugui::absolute({arguments})",
            "Row": f"slugvk::slugui::row({arguments})",
            "Column": f"slugvk::slugui::column({arguments})",
            "Stack": f"slugvk::slugui::stack({arguments})",
            "Rectangle": (
                f"slugvk::slugui::roundedRectangle(slugvk::hashId({cpp_string(stable_id)}), "
                f"slugvk::Paint::solid(slugvk::Color{{}}), slugvk::CornerRadii{{}}, "
                f"slugvk::CornerSmoothing{{}}, {cpp_string(element.name)})"
            ),
            "Shape": (
                f"slugvk::slugui::shape(slugvk::hashId({cpp_string(stable_id)}), {fill_asset}, "
                f"slugvk::Paint::solid(slugvk::Color{{}}), {cpp_string(element.name)})"
            ),
            "Text": (
                f"slugvk::slugui::text(slugvk::hashId({cpp_string(stable_id)}), "
                f"std::string{{}}, slugvk::TextStyle{{}}, {cpp_string(element.name)})"
            ),
        }
        self._line(lines, indent, f"auto {variable} = {factories[element.kind]};")
        shape_visual = f"std::get<slugvk::slugui::ShapeVisual>({variable}.visual)"
        if fill_bounds is not None:
            placement = self._explicit_path_placement(element, fill_placement_attribute)
            if placement is None:
                placement = self._path_placement(element, fill_bounds)
            if placement is not None:
                self._line(lines, indent, f"{shape_visual}.fillPlacement = {placement};")
        if stroke_path_attribute:
            self._line(
                lines, indent,
                f"{shape_visual}.strokeShape = {stroke_asset};",
            )
            placement = self._explicit_path_placement(element, stroke_placement_attribute)
            if placement is None:
                placement = self._path_placement(element, stroke_bounds)
            if placement is not None:
                self._line(lines, indent, f"{shape_visual}.strokePlacement = {placement};")
        for attribute in element.attributes:
            if self._common_attribute(variable, attribute, lines, indent):
                continue
            if self._visual_attribute(element, variable, attribute, lines, indent):
                continue
            raise CompileError(
                f"attribute '{attribute.name}' is not valid on {element.kind}", attribute.token,
            )
        recovered_owner = self._recovered_figma_owner(element)
        if element.kind == "Shape" and recovered_owner is not None:
            base_x = self._fixed_scalar(element, "x") or 0.0
            base_y = self._fixed_scalar(element, "y") or 0.0
            recovered = (
                base_x + recovered_owner[0], base_y + recovered_owner[1],
                recovered_owner[2], recovered_owner[3],
            )
            for member, value in zip(("x", "y", "width", "height"), recovered):
                self._line(
                    lines, indent,
                    f"{variable}.layout.{member} = slugvk::slugui::Length::logical("
                    f"{cpp_float(f'{value:.9g}')});",
                )
        if element.kind == "Text":
            self._emit_text_runs(element, variable, lines, indent)
        child_path = f"{path}/{element.name}" if path else element.name
        for child in element.children:
            child_variable = self._node(child, child_path, lines, indent)
            self._line(lines, indent, f"{variable}.add(std::move({child_variable}));")
        return variable

    def generate(self, source_name: str) -> str:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", self.component.name):
            raise CompileError("component name must be a C++ identifier", self.component.token)
        namespace_parts = self.namespace.split("::") if self.namespace else []
        if any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", part) for part in namespace_parts):
            raise CompileError("namespace must contain C++ identifiers separated by ::")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", self.class_name):
            raise CompileError("generated class name must be a C++ identifier")

        build_lines: list[str] = []
        self._line(build_lines, 2, "(void)atlas;")
        self._line(build_lines, 2, "Built result;")
        for prop in self.component.properties:
            if prop.initial.kind == "reference":
                raise CompileError("property initializers cannot reference another property", prop.initial.token)
            expected = self.PROPERTY_EXPECTED[prop.type_name]
            initial = literal_cpp(prop.initial, expected)
            member = self.property_members[prop.name]
            self._line(
                build_lines, 2,
                f"result.{member} = result.properties.define<{PROPERTY_CPP[prop.type_name]}>("
                f"{cpp_string(prop.name)}, {initial});",
            )
        root_variable = self._node(self.component.root, "", build_lines, 2)
        self._line(build_lines, 2, f"result.root = std::move({root_variable});")
        self._line(build_lines, 2, "return result;")

        class_name = self.class_name
        lines = [
            "#pragma once",
            "",
            '#include "slugvk/slugui.hpp"',
            '#include "slugvk/vector_atlas.hpp"',
            "",
            "#include <cstdint>",
            "#include <string>",
            "#include <utility>",
            "",
            f"// Generated by slugui_compiler.py from "
            f"{source_name.replace(chr(13), ' ').replace(chr(10), ' ')}. Do not edit.",
        ]
        for part in namespace_parts:
            lines.append(f"namespace {part} {{")
        if namespace_parts:
            lines.append("")
        lines.extend([
            f"struct {class_name} {{",
            "public:",
        ])
        for callback, member in sorted(self.callbacks.items()):
            lines.append(
                f"  static inline const slugvk::slugui::CallbackId {member} = "
                f"slugvk::hashId({cpp_string(self.component.name + '.callback.' + callback)});"
            )
        if self.callbacks:
            lines.append("")
        lines.append("  slugvk::slugui::Component component;")
        for prop in self.component.properties:
            lines.append(
                f"  slugvk::slugui::Property<{PROPERTY_CPP[prop.type_name]}> "
                f"{self.property_members[prop.name]};"
            )
        lines.extend([
            "",
            f"  {class_name}() : {class_name}(build(nullptr)) {{}}",
            f"  explicit {class_name}(slugvk::VectorAtlas& atlas) : {class_name}(build(&atlas)) {{}}",
            "",
            "private:",
            "  struct Built {",
            "    slugvk::slugui::PropertyStore properties;",
            "    slugvk::slugui::Element root;",
        ])
        for prop in self.component.properties:
            lines.append(
                f"    slugvk::slugui::Property<{PROPERTY_CPP[prop.type_name]}> "
                f"{self.property_members[prop.name]};"
            )
        lines.extend([
            "  };",
            "",
            "  static Built build(slugvk::VectorAtlas* atlas) {",
            *build_lines,
            "  }",
            "",
            f"  explicit {class_name}(Built built)",
            "      : component(std::move(built.root), std::move(built.properties)),",
        ])
        if self.component.properties:
            for index, prop in enumerate(self.component.properties):
                suffix = "," if index + 1 < len(self.component.properties) else " {}"
                member = self.property_members[prop.name]
                lines.append(f"        {member}(built.{member}){suffix}")
        else:
            lines[-1] = "      : component(std::move(built.root), std::move(built.properties)) {}"
        lines.extend([
            "};",
            "",
        ])
        for part in reversed(namespace_parts):
            lines.append(f"}} // namespace {part}")
        return "\n".join(lines) + "\n"


def compile_text(source: str, source_name: str, namespace: str,
                 class_name: str | None = None) -> str:
    component = parse_source(source)
    header = re.search(r"(?m)^\s*//\s*SlugUI exchange format\s+(\d+)\s*,", source)
    legacy_figma_coordinates = header is not None and int(header.group(1)) == 1
    return Generator(
        component, namespace, class_name, legacy_figma_coordinates).generate(source_name)


def write_atomic(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            output.write(content)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compile a .slugui component into a typed C++ header."
    )
    parser.add_argument("input", type=Path, help="input .slugui file")
    parser.add_argument("-o", "--output", type=Path, help="generated C++ header")
    parser.add_argument("--namespace", default="slugvk::generated", help="generated C++ namespace")
    parser.add_argument("--class-name", help="generated C++ class name override")
    parser.add_argument("--check", action="store_true", help="parse and type-check without writing")
    parser.add_argument("--stdout", action="store_true", help="write generated C++ to stdout")
    arguments = parser.parse_args(argv)
    if not arguments.check and not arguments.stdout and arguments.output is None:
        parser.error("-o/--output is required unless --check or --stdout is used")
    try:
        source = arguments.input.read_text(encoding="utf-8")
        generated = compile_text(
            source, arguments.input.name, arguments.namespace, arguments.class_name)
        if arguments.stdout:
            sys.stdout.write(generated)
        elif not arguments.check:
            write_atomic(arguments.output, generated)
        return 0
    except OSError as error:
        print(f"{arguments.input}: error: {error}", file=sys.stderr)
        return 2
    except CompileError as error:
        if error.token is None:
            location = str(arguments.input)
        else:
            location = f"{arguments.input}:{error.token.line}:{error.token.column}"
        print(f"{location}: error: {error.message}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
