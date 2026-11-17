// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <vector>
#include "supple/diagcap/native.h"
using namespace supple::diagnostics::detail;
TEST_CASE("Windows acknowledged return values are not exception evidence") {
    CHECK_FALSE(windowsFatalStatus(0xffffffffU, 0xffffffffU));
    CHECK_FALSE(windowsFatalStatus(0xc0000005U, 0xc0000005U));
    CHECK_FALSE(windowsFatalStatus(7, 7));
    CHECK(windowsFatalStatus(0xc0000005U, 7));
    CHECK(windowsFatalStatus(0xc0000005U, std::nullopt));
    CHECK_FALSE(windowsFatalStatus(7, std::nullopt));
}

TEST_CASE("the Windows channel hands over Ready before Terminal, and every slot once") {
    const auto bit = [](Message kind) { return 1U << sharedSlot(kind); };
    CHECK(sharedSlot(Message::Ready) != sharedSlot(Message::Terminal));
    CHECK(sharedSlot(Message::Ready) != sharedSlot(Message::Expected));
    CHECK(sharedSlot(Message::Terminal) != sharedSlot(Message::Expected));
    CHECK(nextSharedSlot(0) == -1);
    // A worker that published Ready and then faulted before the supervisor woke: the table
    // address in Ready has to be taken before Terminal is handled.
    const std::uint32_t both = bit(Message::Ready) | bit(Message::Terminal);
    CHECK(nextSharedSlot(both) == static_cast<int>(sharedSlot(Message::Ready)));
    CHECK(nextSharedSlot(both & ~bit(Message::Ready)) ==
          static_cast<int>(sharedSlot(Message::Terminal)));
    // All three pending: each comes out once, Ready, Terminal, Expected.
    std::uint32_t pending = bit(Message::Ready) | bit(Message::Terminal) | bit(Message::Expected);
    std::vector<int> order;
    for (int slot; (slot = nextSharedSlot(pending)) >= 0;) {
        order.push_back(slot);
        pending &= ~(1U << slot);
    }
    CHECK(order == std::vector<int>{static_cast<int>(sharedSlot(Message::Ready)),
                                    static_cast<int>(sharedSlot(Message::Terminal)),
                                    static_cast<int>(sharedSlot(Message::Expected))});
}
