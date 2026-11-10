/**
 * @file nodegraph_value_test.cpp
 * @brief The type registry and `Value`: registration, storage strategy, never-throwing reads,
 *        conversions and hashing.
 *
 * Graph structure is covered by nodegraph_graph_test, compilation by nodegraph_compile_test and
 * execution by nodegraph_eval_test.
 */

#include <coopa/testing/test.h>

#include <glm/glm.hpp>

#include "support/nodegraph_fixtures.h"

COOPA_TEST_SUITE("nodegraph_value");

using namespace coopa::nodegraph;
using nodegraph_fixtures::BigPayload;
using nodegraph_fixtures::Tracked;
using coopa::test::expect;
using coopa::test::expect_near;

COOPA_TEST(registering_a_type_twice_returns_the_same_id) {
    const auto& t = nodegraph_fixtures::setup();
    TypeId again = TypeRegistry::instance().add<float>("Float");
    expect(again == t.f,
           "nodegraph: re-registering a name returns the existing id -- a consumer may call its "
           "registration function more than once without invalidating live graphs");
    expect(TypeRegistry::instance().find("Float") == t.f, "nodegraph: find() agrees with add()");
    expect(TypeRegistry::instance().find("NoSuchType") == k_invalid_type,
           "nodegraph: an unregistered name is k_invalid_type, not a crash");
}

COOPA_TEST(an_invalid_type_id_yields_a_null_typeinfo_rather_than_a_wild_call) {
    nodegraph_fixtures::setup();
    const TypeInfo& info = TypeRegistry::instance().info(k_invalid_type);
    expect(info.size == 0 && info.destroy == nullptr,
           "nodegraph: info() on an invalid id returns an all-null TypeInfo, so a caller that "
           "forgot to check gets a predictable no-op instead of calling through garbage");
    expect(TypeRegistry::instance().name_of(k_invalid_type) == "<invalid>",
           "nodegraph: an invalid id names itself rather than returning an empty string");
}

COOPA_TEST(a_small_type_stays_inline_and_a_large_one_shares_a_block) {
    const auto& t = nodegraph_fixtures::setup();

    Value small = Value::make(t.v, glm::vec3(1.0f, 2.0f, 3.0f));
    Value small_copy = small;
    expect(!small.shared() && !small_copy.shared(),
           "nodegraph: a vec3 fits the inline buffer, so copies are independent and nothing is shared");

    Value big = Value::make(t.big, BigPayload(5));
    expect(!big.shared(), "nodegraph: a freshly made large value is not yet shared");
    Value big_copy = big;
    expect(big.shared() && big_copy.shared(),
           "nodegraph: copying a large value shares one block -- this is what makes socket fan-out "
           "cost a refcount bump rather than a deep copy");
    expect(big.data() == big_copy.data(), "nodegraph: a shared copy points at the same payload");
}

COOPA_TEST(copying_a_shared_value_does_not_deep_copy_but_writing_through_one_does) {
    const auto& t = nodegraph_fixtures::setup();
    nodegraph_fixtures::reset_counters();

    Value original = Value::make(t.tracked, Tracked(11));
    int after_make = Tracked::copies().load();

    Value a = original;
    Value b = original;
    expect(Tracked::copies().load() == after_make,
           "nodegraph: two copies of a shared value performed ZERO deep copies -- the property the "
           "whole slot design rests on");

    int* dummy = nullptr;
    (void)dummy;
    a.mutable_data();   // Copy-on-write: this one must clone.
    expect(Tracked::copies().load() == after_make + 1,
           "nodegraph: writing through a shared value clones exactly once");
    expect(!a.shared(), "nodegraph: after a clone the writer no longer shares");
    expect(b.get<Tracked>().tag == 11, "nodegraph: the other holder still sees the original value");
}

COOPA_TEST(as_returns_the_fallback_on_a_mismatch_and_never_throws) {
    const auto& t = nodegraph_fixtures::setup();
    Value f = Value::make(t.f, 2.5f);

    expect_near(f.as<float>(-1.0f), 2.5f, 1e-6f, "nodegraph: a matching read returns the value");
    // A double is 8 bytes against Float's 4, so this is the mismatch path.
    expect(f.as<double>(-1.0) == -1.0,
           "nodegraph: a mismatched read returns the fallback rather than reinterpreting bytes -- "
           "EventValue's contract, because graph data is authored and may be wrong");

    Value empty;
    expect(empty.as<float>(7.0f) == 7.0f, "nodegraph: reading an empty value returns the fallback");
    expect(empty.empty() && empty.type() == k_invalid_type, "nodegraph: a default Value is empty");
    expect(empty.data() == nullptr, "nodegraph: an empty value has no payload pointer");
}

COOPA_TEST(make_default_builds_a_zeroed_value_of_the_right_type) {
    const auto& t = nodegraph_fixtures::setup();
    Value f = Value::make_default(t.f);
    expect(!f.empty() && f.type() == t.f, "nodegraph: make_default produces a live value");
    expect_near(f.as<float>(-1.0f), 0.0f, 1e-6f, "nodegraph: a default float is zero");

    Value bad = Value::make_default(k_invalid_type);
    expect(bad.empty(),
           "nodegraph: make_default on an unregistered id stays empty rather than allocating "
           "zero bytes and pretending to hold something");
}

COOPA_TEST(a_registered_conversion_applies_and_a_missing_one_reports_rather_than_guessing) {
    const auto& t = nodegraph_fixtures::setup();
    const auto& reg = TypeRegistry::instance();

    expect(reg.can_convert(t.i, t.f), "nodegraph: Int -> Float was registered");
    expect(reg.can_convert(t.f, t.v), "nodegraph: Float -> Vector was registered");
    expect(reg.can_convert(t.f, t.f), "nodegraph: a type always converts to itself");
    expect(!reg.can_convert(t.v, t.big),
           "nodegraph: an unregistered pair reports false instead of attempting a bit copy");

    Value i = Value::make(t.i, int32_t(4));
    Value as_float = i.converted_to(t.f);
    expect(as_float.type() == t.f, "nodegraph: the converted value carries the target type");
    expect_near(as_float.as<float>(-1.0f), 4.0f, 1e-6f, "nodegraph: Int 4 converts to Float 4");

    Value as_vec = as_float.converted_to(t.v);
    expect_near(as_vec.as<glm::vec3>(glm::vec3(0.0f)).y, 4.0f, 1e-6f,
                "nodegraph: Float -> Vector broadcasts, as Blender's implicit widening does");

    Value impossible = Value::make(t.v, glm::vec3(1.0f)).converted_to(t.big);
    expect(impossible.empty(),
           "nodegraph: an unavailable conversion yields an empty Value, which the compiler turns "
           "into a named error rather than a silent zero");

    Value same = as_float.converted_to(t.f);
    expect_near(same.as<float>(-1.0f), 4.0f, 1e-6f,
                "nodegraph: converting to the same type is a plain copy, which is what lets the "
                "compiler apply conversions unconditionally");
}

COOPA_TEST(equal_values_hash_equal_and_different_ones_do_not) {
    const auto& t = nodegraph_fixtures::setup();
    Value a = Value::make(t.f, 1.5f);
    Value b = Value::make(t.f, 1.5f);
    Value c = Value::make(t.f, 1.6f);

    expect(a.equals(b), "nodegraph: two equal floats compare equal");
    expect(!a.equals(c), "nodegraph: two different floats do not");
    expect(a.hash() == b.hash(), "nodegraph: equal values hash equal -- change detection needs this");
    expect(a.hash() != c.hash(), "nodegraph: different values hash differently");

    Value as_int = Value::make(t.i, int32_t(1));
    expect(!a.equals(as_int),
           "nodegraph: values of different types are never equal, whatever their bytes");
    expect(a.hash() != as_int.hash(), "nodegraph: the type id is folded into the hash");
}

COOPA_TEST(moving_a_value_leaves_the_source_empty) {
    const auto& t = nodegraph_fixtures::setup();

    Value inline_src = Value::make(t.f, 3.0f);
    Value inline_dst = std::move(inline_src);
    expect(inline_src.empty(), "nodegraph: a moved-from inline value is empty, not a stale duplicate");
    expect_near(inline_dst.as<float>(-1.0f), 3.0f, 1e-6f, "nodegraph: the move carried the value");

    Value big_src = Value::make(t.big, BigPayload(9));
    const void* payload = big_src.data();
    Value big_dst = std::move(big_src);
    expect(big_src.empty(), "nodegraph: a moved-from shared value is empty");
    expect(big_dst.data() == payload,
           "nodegraph: moving a shared value hands over the same block rather than reallocating");
    expect(!big_dst.shared(), "nodegraph: a move does not bump the refcount");
}

COOPA_TEST(resetting_and_reassigning_destroys_the_old_contents) {
    const auto& t = nodegraph_fixtures::setup();
    nodegraph_fixtures::reset_counters();

    {
        Value v = Value::make(t.big, BigPayload(3));
        v.reset();
        expect(v.empty() && v.data() == nullptr, "nodegraph: reset() leaves an empty value");
    }

    Value a = Value::make(t.f, 1.0f);
    a = Value::make(t.f, 2.0f);
    expect_near(a.as<float>(-1.0f), 2.0f, 1e-6f, "nodegraph: assignment replaces the contents");

    Value self = Value::make(t.f, 5.0f);
    const Value& alias = self;
    self = alias;   // Self-assignment must not destroy then read.
    expect_near(self.as<float>(-1.0f), 5.0f, 1e-6f,
                "nodegraph: self-assignment is a no-op rather than a use-after-destroy");
}
