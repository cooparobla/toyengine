#include <toyengine/core/loading_config.h>

#include <algorithm>
#include <cctype>

namespace toy {
namespace core {

LoadingCorner parse_loading_corner(const std::string& value) {
    std::string s = value;
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "bottom_left" || s == "bottomleft")  return LoadingCorner::BottomLeft;
    if (s == "top_right" || s == "topright")      return LoadingCorner::TopRight;
    if (s == "top_left" || s == "topleft")        return LoadingCorner::TopLeft;
    return LoadingCorner::BottomRight;
}

const char* loading_corner_name(LoadingCorner corner) {
    switch (corner) {
        case LoadingCorner::BottomLeft:  return "bottom_left";
        case LoadingCorner::TopRight:    return "top_right";
        case LoadingCorner::TopLeft:     return "top_left";
        case LoadingCorner::BottomRight: break;
    }
    return "bottom_right";
}

} // namespace core
} // namespace toy
