// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The take owner (ADR-0097): which item holds a take, and what an item holds.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "supple/model/session.h"
#include "supple/model/take_owner.h"

using namespace supple;
using namespace supple::model;

namespace {

struct Fixture {
    Session s;
    Id track, first, second, a, b, c, loose;
};

// Two items on one channel: the first holds takes a and b, the second c.
// `loose` exists in a preview only, held by nothing (an invalid session).
Fixture fixture() {
    Fixture f;
    EditTxn t(Session::create(), 1);
    Track tr;
    tr.parent = kMaster;
    f.track = t.create(tr);
    t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(f.track); });
    f.a = t.create(Take{});
    f.b = t.create(Take{});
    f.c = t.create(Take{});
    Item item;
    item.track = f.track;
    item.length = 4;
    item.takes = {f.a, f.b};
    item.activeTake = f.b;
    f.first = t.create(item);
    item.takes = {f.c};
    item.activeTake = f.c;
    item.position = 8;
    f.second = t.create(item);
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? "" : s.error().toString()));
    f.s = *s;
    return f;
}

// Every invariant message in `e`, one per line (several come as details).
std::string messages(const supple::Error& e) {
    std::string out = e.message + "\n";
    for (const auto& d : e.details) out += d.message + "\n";
    return out;
}

}  // namespace

TEST_CASE("take owner: each take resolves to the item that holds it") {
    const Fixture f = fixture();
    CHECK(takeOwner(f.s, f.a) == f.first);
    CHECK(takeOwner(f.s, f.b) == f.first);
    CHECK(takeOwner(f.s, f.c) == f.second);
    // Not a take, or a take id that does not exist: no owner.
    CHECK_FALSE(takeOwner(f.s, f.track));
    CHECK_FALSE(takeOwner(f.s, Id{Kind::Take, 999}));
}

TEST_CASE("take owner: an owner's takes, in its order") {
    const Fixture f = fixture();
    const std::vector<Id>* first = ownerTakes(f.s, f.first);
    REQUIRE(first);
    CHECK(*first == std::vector<Id>{f.a, f.b});
    REQUIRE(ownerTakes(f.s, f.second));
    CHECK(*ownerTakes(f.s, f.second) == std::vector<Id>{f.c});
    // Not an owner: a track, a take, or an item id that does not exist.
    CHECK_FALSE(ownerTakes(f.s, f.track));
    CHECK_FALSE(ownerTakes(f.s, f.a));
    CHECK_FALSE(ownerTakes(f.s, Id{Kind::Item, 999}));
}

TEST_CASE("take owner: the one-pass index answers as takeOwner does") {
    const Fixture f = fixture();
    const TakeOwners owners(f.s);
    CHECK(owners.size() == 3);
    for (const Id take : {f.a, f.b, f.c}) CHECK(owners.of(take) == takeOwner(f.s, take));
    CHECK_FALSE(owners.of(Id{Kind::Take, 999}));
    CHECK(TakeOwners(Session::create()).size() == 0);
}

TEST_CASE("take owner: in a broken session, the first owner in id order; a loose take has none") {
    const Fixture f = fixture();
    EditTxn t(f.s, 2);
    t.modify<Item>(f.second, [&](Item& it) { it.takes.push_back(f.a); });
    const Id loose = t.create(Take{});
    const Session broken = t.preview();
    CHECK(takeOwner(broken, f.a) == f.first);
    CHECK(TakeOwners(broken).of(f.a) == f.first);
    CHECK_FALSE(takeOwner(broken, loose));
    CHECK_FALSE(TakeOwners(broken).of(loose));
    // The invariants name both faults, through the same answers.
    const Status st = checkInvariants(broken);
    REQUIRE_FALSE(st);
    const std::string text = messages(st.error());
    CHECK(text.find(f.a.str() + " also belongs to " + f.first.str()) != std::string::npos);
    CHECK(text.find(loose.str() + " belongs to no item") != std::string::npos);
}

TEST_CASE("take owner: a take listed twice by one item is still a fault") {
    const Fixture f = fixture();
    EditTxn t(f.s, 2);
    t.modify<Item>(f.second, [&](Item& it) { it.takes.push_back(f.c); });
    const Status st = checkInvariants(t.preview());
    REQUIRE_FALSE(st);
    CHECK(messages(st.error()).find(f.c.str() + " also belongs to " + f.second.str()) !=
          std::string::npos);
    CHECK(takeOwner(t.preview(), f.c) == f.second);
}
