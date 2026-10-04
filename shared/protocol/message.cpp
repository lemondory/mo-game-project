#include "message.hpp"

#include <algorithm>

namespace mo::protocol {

bool valid(MessageId id) {
    const auto& known = enum_traits<MessageId>::values;
    return std::find(known.begin(), known.end(), id) != known.end();
}

} // mo::protocol 네임스페이스 끝
