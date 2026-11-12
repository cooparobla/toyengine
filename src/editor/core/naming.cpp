#include "editor/core/naming.h"

namespace toy {
namespace editor {

std::string snake_case(const std::string& s) {
    std::string out;
    auto sep = [&] { if (!out.empty() && out.back() != '_') out += '_'; };
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isalnum(c)) {
            const bool upper = std::isupper(c) != 0;
            const bool prev_lower = i > 0 && (std::islower(static_cast<unsigned char>(s[i - 1])) || std::isdigit(static_cast<unsigned char>(s[i - 1])));
            const bool next_lower = i + 1 < s.size() && std::islower(static_cast<unsigned char>(s[i + 1]));
            const bool prev_upper = i > 0 && std::isupper(static_cast<unsigned char>(s[i - 1]));
            // "PointLight" -> point_light; "HTTPServer" -> http_server
            if (upper && (prev_lower || (prev_upper && next_lower))) sep();
            out += static_cast<char>(std::tolower(c));
        } else {
            sep();
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

} // namespace editor
} // namespace toy
