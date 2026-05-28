#pragma once

#include <cstddef>
#include <string_view>

namespace keyconv {

enum class ConvertedChartMarkerKind {
    None,
    KeyWeaver,
    AKey,
    ToKeyC,
    KeyC,
};

namespace detail {

template <typename Char>
bool isAsciiDigit(Char ch) {
    return ch >= static_cast<Char>('0') && ch <= static_cast<Char>('9');
}

template <typename Char>
bool isAsciiAlpha(Char ch) {
    return (ch >= static_cast<Char>('a') && ch <= static_cast<Char>('z')) ||
           (ch >= static_cast<Char>('A') && ch <= static_cast<Char>('Z'));
}

template <typename Char>
bool isAsciiAlphaNum(Char ch) {
    return isAsciiAlpha(ch) || isAsciiDigit(ch);
}

template <typename Char>
Char lowerAscii(Char ch) {
    if (ch >= static_cast<Char>('A') && ch <= static_cast<Char>('Z')) {
        return static_cast<Char>(ch - static_cast<Char>('A') + static_cast<Char>('a'));
    }
    return ch;
}

template <typename Char>
bool boundaryBefore(std::basic_string_view<Char> text, std::size_t index) {
    return index == 0 || !isAsciiAlphaNum(text[index - 1]);
}

template <typename Char>
bool boundaryAfter(std::basic_string_view<Char> text, std::size_t index) {
    return index >= text.size() || !isAsciiAlphaNum(text[index]);
}

template <typename Char>
std::size_t skipSeparators(std::basic_string_view<Char> text, std::size_t index) {
    while (index < text.size() && !isAsciiAlphaNum(text[index])) {
        ++index;
    }
    return index;
}

template <typename Char>
std::size_t readOneOrTwoDigits(std::basic_string_view<Char> text, std::size_t index) {
    const std::size_t begin = index;
    while (index < text.size() && isAsciiDigit(text[index]) && index - begin < 2) {
        ++index;
    }
    if (index == begin) {
        return std::basic_string_view<Char>::npos;
    }
    if (index < text.size() && isAsciiDigit(text[index])) {
        return std::basic_string_view<Char>::npos;
    }
    return index;
}

template <typename Char>
bool matchesAsciiLiteral(std::basic_string_view<Char> text, std::size_t index, const char* literal) {
    for (std::size_t offset = 0; literal[offset] != '\0'; ++offset) {
        if (index + offset >= text.size()) {
            return false;
        }
        if (lowerAscii(text[index + offset]) != static_cast<Char>(literal[offset])) {
            return false;
        }
    }
    return true;
}

template <typename Char>
bool containsAsciiLiteral(std::basic_string_view<Char> text, const char* literal) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (matchesAsciiLiteral(text, index, literal)) {
            return true;
        }
    }
    return false;
}

template <typename Char>
bool hasAKeyMarker(std::basic_string_view<Char> text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (lowerAscii(text[index]) != static_cast<Char>('a') || !boundaryBefore(text, index)) {
            continue;
        }
        std::size_t cursor = readOneOrTwoDigits(text, index + 1);
        if (cursor == std::basic_string_view<Char>::npos || cursor >= text.size()) {
            continue;
        }
        if (lowerAscii(text[cursor]) == static_cast<Char>('k') && boundaryAfter(text, cursor + 1)) {
            return true;
        }
    }
    return false;
}

template <typename Char>
bool hasToKeyCMarker(std::basic_string_view<Char> text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (!isAsciiDigit(text[index]) || !boundaryBefore(text, index)) {
            continue;
        }
        std::size_t cursor = readOneOrTwoDigits(text, index);
        if (cursor == std::basic_string_view<Char>::npos) {
            continue;
        }
        cursor = skipSeparators(text, cursor);
        if (cursor + 1 >= text.size() ||
            lowerAscii(text[cursor]) != static_cast<Char>('t') ||
            lowerAscii(text[cursor + 1]) != static_cast<Char>('o')) {
            continue;
        }
        cursor = skipSeparators(text, cursor + 2);
        cursor = readOneOrTwoDigits(text, cursor);
        if (cursor == std::basic_string_view<Char>::npos) {
            continue;
        }
        cursor = skipSeparators(text, cursor);
        if (cursor < text.size() && lowerAscii(text[cursor]) == static_cast<Char>('c')) {
            ++cursor;
        }
        if (boundaryAfter(text, cursor)) {
            return true;
        }
    }
    return false;
}

template <typename Char>
bool hasKeyCMarker(std::basic_string_view<Char> text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (!isAsciiDigit(text[index]) || !boundaryBefore(text, index)) {
            continue;
        }
        std::size_t cursor = readOneOrTwoDigits(text, index);
        if (cursor == std::basic_string_view<Char>::npos) {
            continue;
        }
        cursor = skipSeparators(text, cursor);
        if (cursor >= text.size() || lowerAscii(text[cursor]) != static_cast<Char>('k')) {
            continue;
        }
        cursor = skipSeparators(text, cursor + 1);
        cursor = readOneOrTwoDigits(text, cursor);
        if (cursor == std::basic_string_view<Char>::npos) {
            continue;
        }
        cursor = skipSeparators(text, cursor);
        if (cursor < text.size() && lowerAscii(text[cursor]) == static_cast<Char>('c') &&
            boundaryAfter(text, cursor + 1)) {
            return true;
        }
    }
    return false;
}

}  // namespace detail

template <typename Char>
ConvertedChartMarkerKind convertedChartMarkerKind(std::basic_string_view<Char> text) {
    if (detail::containsAsciiLiteral(text, "keyweaver")) {
        return ConvertedChartMarkerKind::KeyWeaver;
    }
    if (detail::hasAKeyMarker(text)) {
        return ConvertedChartMarkerKind::AKey;
    }
    if (detail::hasToKeyCMarker(text)) {
        return ConvertedChartMarkerKind::ToKeyC;
    }
    if (detail::hasKeyCMarker(text)) {
        return ConvertedChartMarkerKind::KeyC;
    }
    return ConvertedChartMarkerKind::None;
}

template <typename Char>
bool hasConvertedChartMarker(std::basic_string_view<Char> text) {
    return convertedChartMarkerKind(text) != ConvertedChartMarkerKind::None;
}

inline std::string_view convertedChartMarkerLabel(ConvertedChartMarkerKind kind) {
    switch (kind) {
        case ConvertedChartMarkerKind::KeyWeaver:
            return "KeyWeaver marker";
        case ConvertedChartMarkerKind::AKey:
            return "aNK marker";
        case ConvertedChartMarkerKind::ToKeyC:
            return "NtoN(c) marker";
        case ConvertedChartMarkerKind::KeyC:
            return "NKNC marker";
        case ConvertedChartMarkerKind::None:
            return "no marker";
    }
    return "no marker";
}

}  // namespace keyconv
