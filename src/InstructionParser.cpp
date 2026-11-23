#include "InstructionParser.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#include "Utils.h"

namespace {

bool isIdentifier(const std::string& text) {
    if (text.empty() || !(std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_')) {
        return false;
    }
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; });
}

// True if text is non-empty and all decimal digits, or all hexadecimal digits when hex is set.
bool isDigits(const std::string& text, bool hex) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [hex](unsigned char c) {
               return (hex ? std::isxdigit(c) : std::isdigit(c)) != 0;
           });
}

// A decimal number clamped to [0, limit]. A leading minus sign clamps it to 0.
std::optional<uint64_t> parseNumber(const std::string& text, uint64_t limit) {
    bool negative = !text.empty() && text[0] == '-';
    std::string digits = negative ? text.substr(1) : text;
    if (!isDigits(digits, false)) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (char digit : digits) {
        value = std::min<uint64_t>(value * 10 + (digit - '0'), limit);
    }
    return negative ? 0 : value;
}

std::optional<uint16_t> parseValue(const std::string& text) {
    std::optional<uint64_t> value = parseNumber(text, UINT16_MAX);
    if (!value) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(*value);
}

// A hexadecimal address, with or without a 0x prefix.
std::optional<uint64_t> parseAddress(const std::string& text) {
    std::string digits = text;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits = digits.substr(2);
    }
    if (digits.size() > 16 || !isDigits(digits, true)) {
        return std::nullopt;
    }
    return std::stoull(digits, nullptr, 16);
}

std::optional<Operand> parseOperand(const std::string& text) {
    Operand operand;
    if (std::optional<uint16_t> value = parseValue(text)) {
        operand.value = *value;
        return operand;
    }
    if (isIdentifier(text)) {
        operand.isVariable = true;
        operand.variable = text;
        return operand;
    }
    return std::nullopt;
}

// Splits text at the semicolons outside quotes, parentheses and brackets, and drops empty parts.
// Returns nullopt if a quote, parenthesis or bracket is left open, or one is closed that wasn't open.
std::optional<std::vector<std::string>> splitStatements(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    bool inQuotes = false;
    int depth = 0;
    for (char c : text) {
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (!inQuotes) {
            if (c == '(' || c == '[') {
                ++depth;
            } else if ((c == ')' || c == ']') && --depth < 0) {
                return std::nullopt;
            } else if (c == ';' && depth == 0) {
                parts.push_back(trim(current));
                current.clear();
                continue;
            }
        }
        current += c;
    }
    if (inQuotes || depth != 0) {
        return std::nullopt;
    }
    parts.push_back(trim(current));
    parts.erase(std::remove(parts.begin(), parts.end(), std::string()), parts.end());
    return parts;
}

// The text inside "( ... )", trimmed, or nullopt if text isn't wrapped in parentheses.
std::optional<std::string> insideParentheses(const std::string& text) {
    if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
        return std::nullopt;
    }
    return trim(text.substr(1, text.size() - 2));
}

// The arguments after a keyword: "(a, b)" or "a b".
std::optional<std::vector<std::string>> parseArguments(const std::string& text) {
    if (text.empty() || text[0] != '(') {
        return splitWords(text);
    }
    std::optional<std::string> inside = insideParentheses(text);
    if (!inside) {
        return std::nullopt;
    }
    std::vector<std::string> arguments;
    std::istringstream stream(*inside);
    std::string argument;
    while (std::getline(stream, argument, ',')) {
        arguments.push_back(trim(argument));
    }
    return arguments;
}

std::optional<std::vector<Instruction>> parseStatements(const std::string& text, int depth);

// PRINT's argument: "text", var, or "text" + var.
std::optional<Instruction> parsePrint(const std::string& text) {
    std::optional<std::string> inside = insideParentheses(text);
    if (!inside) {
        return std::nullopt;
    }
    Instruction print;
    print.type = InstructionType::PRINT;
    if (isIdentifier(*inside)) {
        print.variable = *inside;
        return print;
    }
    if (inside->empty() || (*inside)[0] != '"') {
        return std::nullopt;
    }
    size_t close = inside->find('"', 1);
    if (close == std::string::npos) {
        return std::nullopt;
    }
    print.message = inside->substr(1, close - 1);
    std::string rest = trim(inside->substr(close + 1));
    if (!rest.empty()) {
        std::string variable = trim(rest.substr(1));
        if (rest[0] != '+' || !isIdentifier(variable)) {
            return std::nullopt;
        }
        print.variable = variable;
    }
    return print;
}

// FOR's arguments: [statements], repeats. depth is the number of loops around this one.
std::optional<Instruction> parseFor(const std::string& text, int depth) {
    std::optional<std::string> inside = insideParentheses(text);
    if (depth >= kMaxForNesting || !inside || inside->empty() || (*inside)[0] != '[') {
        return std::nullopt;
    }
    // Find the bracket that closes the body.
    size_t close = std::string::npos;
    int brackets = 0;
    bool inQuotes = false;
    for (size_t i = 0; i < inside->size() && close == std::string::npos; ++i) {
        char c = (*inside)[i];
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (!inQuotes && c == '[') {
            ++brackets;
        } else if (!inQuotes && c == ']' && --brackets == 0) {
            close = i;
        }
    }
    if (close == std::string::npos) {
        return std::nullopt;
    }
    std::string rest = trim(inside->substr(close + 1));
    if (rest.empty() || rest[0] != ',') {
        return std::nullopt;
    }
    std::optional<uint16_t> repeats = parseValue(trim(rest.substr(1)));
    std::optional<std::vector<Instruction>> body = parseStatements(inside->substr(1, close - 1), depth + 1);
    if (!repeats || *repeats == 0 || !body || body->empty()) {
        return std::nullopt;
    }
    Instruction loop;
    loop.type = InstructionType::FOR;
    loop.repeats = *repeats;
    loop.body = std::move(*body);
    return loop;
}

std::optional<Instruction> parseStatement(const std::string& text, int depth) {
    size_t keywordEnd = 0;
    while (keywordEnd < text.size() && std::isalpha(static_cast<unsigned char>(text[keywordEnd]))) {
        ++keywordEnd;
    }
    std::string keyword = toLower(text.substr(0, keywordEnd));
    std::string rest = trim(text.substr(keywordEnd));
    if (keyword == "print") {
        return parsePrint(rest);
    }
    if (keyword == "for") {
        return parseFor(rest, depth);
    }

    std::optional<std::vector<std::string>> found = parseArguments(rest);
    if (!found) {
        return std::nullopt;
    }
    const std::vector<std::string>& arguments = *found;
    Instruction instruction;

    if (keyword == "declare" && arguments.size() == 2 && isIdentifier(arguments[0])) {
        std::optional<uint16_t> value = parseValue(arguments[1]);
        if (value) {
            instruction.type = InstructionType::DECLARE;
            instruction.variable = arguments[0];
            instruction.first.value = *value;
            return instruction;
        }
    } else if ((keyword == "add" || keyword == "subtract") && arguments.size() == 3 && isIdentifier(arguments[0])) {
        std::optional<Operand> first = parseOperand(arguments[1]);
        std::optional<Operand> second = parseOperand(arguments[2]);
        if (first && second) {
            instruction.type = keyword == "add" ? InstructionType::ADD : InstructionType::SUBTRACT;
            instruction.variable = arguments[0];
            instruction.first = *first;
            instruction.second = *second;
            return instruction;
        }
    } else if (keyword == "sleep" && arguments.size() == 1) {
        std::optional<uint64_t> ticks = parseNumber(arguments[0], UINT8_MAX);
        if (ticks) {
            instruction.type = InstructionType::SLEEP;
            instruction.sleepTicks = static_cast<uint8_t>(*ticks);
            return instruction;
        }
    } else if (keyword == "read" && arguments.size() == 2 && isIdentifier(arguments[0])) {
        std::optional<uint64_t> address = parseAddress(arguments[1]);
        if (address) {
            instruction.type = InstructionType::READ;
            instruction.variable = arguments[0];
            instruction.address = *address;
            return instruction;
        }
    } else if (keyword == "write" && arguments.size() == 2) {
        std::optional<uint64_t> address = parseAddress(arguments[0]);
        std::optional<Operand> value = parseOperand(arguments[1]);
        if (address && value) {
            instruction.type = InstructionType::WRITE;
            instruction.address = *address;
            instruction.first = *value;
            return instruction;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<Instruction>> parseStatements(const std::string& text, int depth) {
    std::optional<std::vector<std::string>> parts = splitStatements(text);
    if (!parts) {
        return std::nullopt;
    }
    std::vector<Instruction> statements;
    for (const std::string& part : *parts) {
        std::optional<Instruction> statement = parseStatement(part, depth);
        if (!statement) {
            return std::nullopt;
        }
        statements.push_back(std::move(*statement));
    }
    return statements;
}

} // namespace

std::optional<ScreenCreateArgs> parseScreenCreate(const std::string& text) {
    const char* whitespace = " \t";
    std::string rest = trim(text);
    size_t nameEnd = rest.find_first_of(whitespace);
    if (nameEnd == std::string::npos) {
        return std::nullopt;
    }
    ScreenCreateArgs args;
    args.name = rest.substr(0, nameEnd);
    rest = trim(rest.substr(nameEnd));
    if (!rest.empty() && rest[0] != '"') {
        size_t sizeEnd = rest.find_first_of(whitespace);
        if (sizeEnd == std::string::npos) {
            return std::nullopt;
        }
        args.sizeText = rest.substr(0, sizeEnd);
        rest = trim(rest.substr(sizeEnd));
    }
    if (rest.size() < 2 || rest.front() != '"' || rest.back() != '"') {
        return std::nullopt;
    }
    std::string quoted = rest.substr(1, rest.size() - 2);
    for (size_t i = 0; i < quoted.size(); ++i) {
        if (quoted[i] == '\\' && i + 1 < quoted.size() && quoted[i + 1] == '"') {
            ++i; // \" stands for "
        }
        args.instructions += quoted[i];
    }
    return args;
}

std::optional<std::vector<Instruction>> parseProgram(const std::string& text) {
    std::optional<std::vector<Instruction>> statements = parseStatements(text, 0);
    if (!statements || statements->empty() || statements->size() > kMaxProgramStatements) {
        return std::nullopt;
    }
    return statements;
}
