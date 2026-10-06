// Protocol Buffers - Google's data interchange format
// Copyright 2026 Google LLC.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// The process-wide plumbing shared by every gtest-based conformance suite.
//
// ConformanceEnvironment is the process-global state of a conformance test
// binary.  It owns the testee connection and the TestManager, records
// statistics, and checks or regenerates the failure list at the end of the
// run.  Exactly one is installed per test binary, normally by
// test_environment_main.cc from command-line flags (see
// test_environment_flags.h).  Install() hooks it into gtest.  Nothing in this
// header is meant for the conformance suites themselves.
//
// Everything here is single-threaded.  Use it only from gtest's main thread:
// test bodies, fixtures and the environment hooks.  The global environment is
// a plain pointer and TestManager isn't thread-safe, so tests must not create
// or run conformance Tests from other threads.  Yields() only reads the
// failure list; the environment is what tallies the outcomes, from gtest's
// listener hooks.
//
// --gtest_repeat is not supported.  The testee connection rejects a test name
// it has already run, and the TestManager counts each test once, so a
// repeated run would crash or report wrong statistics.

#ifndef GOOGLE_PROTOBUF_CONFORMANCE_TEST_ENVIRONMENT_H__
#define GOOGLE_PROTOBUF_CONFORMANCE_TEST_ENVIRONMENT_H__

#include <memory>
#include <string>
#include <vector>

#include "google/protobuf/descriptor.pb.h"
#include <gtest/gtest.h>
#include "absl/base/nullability.h"
#include "absl/types/optional.h"
#include "conformance/result_record.h"
#include "conformance/test_manager.h"
#include "conformance/test_runner.h"
#include "conformance/testee.h"

namespace google {
namespace protobuf {
namespace conformance {
namespace internal {

// Options for a ConformanceEnvironment.  Normally populated from command-line
// flags by test_environment_main.cc (see OptionsFromFlags() in
// test_environment_flags.h).
struct ConformanceEnvironmentOptions {
  // Exactly one of `runner`, `owned_runner` or `testee_binary` must be set.
  //
  // A caller-owned runner.  It must outlive the environment.  Used for mocks
  // and by the transitional merged runner.
  ConformanceTestRunner* absl_nullable runner = nullptr;

  // A runner the environment takes ownership of, for example an in-process
  // testee that needs shutting down.  It is destroyed in TearDown(), after the
  // failure-list checks.  The environment moves it out of the options, so
  // ConformanceEnvironment::options().owned_runner is always null.
  std::unique_ptr<ConformanceTestRunner> owned_runner;

  // A testee executable to spawn (via ForkPipeRunner) with `testee_args`.
  // Like `owned_runner`, it is shut down in TearDown().
  std::string testee_binary;
  std::vector<std::string> testee_args;

  // Failure list files to load.  All of them are loaded into a single
  // TestManager, so entries must not overlap between files.  SetUp() fails
  // fatally if they do.
  std::vector<std::string> failure_list_files;

  // The highest priority level whose failures count as failures: 0 for P0, 1
  // for P1 (see TestPriority in testee.h).  Failures of tests above it are
  // tolerated and recorded as such.  A listed test is the exception: its
  // failure counts as an expected failure whatever its priority.  The default,
  // kEnforceAllPriorities, enforces every priority.  --enforcement_level sets
  // it explicitly.  0 is what conformance_test_runner does without
  // --enforce_recommended.
  int enforcement_level = kEnforceAllPriorities;

  // The maximum stable edition to test.  Tests for messages of a newer edition
  // are skipped.  EDITION_UNSTABLE tests run whenever this is EDITION_2023 or
  // newer, as on the conformance_test_runner command line.  The default runs
  // only proto2 and proto3 tests, like conformance_test_runner's.
  //
  // Values below EDITION_PROTO3, such as EDITION_UNKNOWN (the
  // conformance_test_runner default), are meaningless, since proto2 and proto3
  // tests always run.  The environment's constructor clamps them to
  // EDITION_PROTO3.  That is the one place the clamp lives.
  Edition maximum_edition = EDITION_PROTO3;

  // When true, only performance tests run; when false, only regular tests run.
  // Because the two kinds of tests are mutually exclusive within one run, a
  // performance run needs its own failure list, like conformance_test_runner's
  // *_performance.txt lists: with a shared list, every regular test's entry
  // would be "unseen" (and dropped by `fix`).
  //
  // TODO: b/410126673 - This in-binary mode is transitional.  It only mirrors
  // conformance_test_runner's --performance flag until the performance tests
  // are a suite of their own, which BUILD files enable or disable like any
  // other suite.  Remove it together with the --performance flag at that
  // point.
  bool performance = false;

  // Rewrites the failure list at the end of the run.  Entries that no longer
  // fail are dropped.  New failures are added with their messages.
  //
  // Refused when only a subset of the binary's tests ran, because of
  // --gtest_filter, sharding or --gtest_fail_fast.  A test failure is reported
  // and nothing is written, since the entries of the tests that didn't run
  // would otherwise be dropped as "unseen".
  bool fix = false;

  // Where to write the fixed failure list.  Required when `fix` is set and
  // `failure_list_files` doesn't have exactly one entry.  Otherwise defaults
  // to that single file under $BUILD_WORKSPACE_DIRECTORY, that is the source
  // file under `bazel run`.
  std::string fix_output_file;

  // Whether TearDown() fails the run if some expected failures were never
  // seen.  The check is skipped automatically when only a subset of the
  // binary's tests ran, for example because of --gtest_filter or test
  // sharding, since a partial run can't see every expected failure.  The
  // conformance_test_runner --test flag behaves the same way.
  //
  // The merged runner sets this to false.  It reports the unmatched entries
  // itself (see TestManager::UnmatchedExpectedFailures).
  bool check_unseen_expected_failures = true;
};

// A snapshot of the TestManager's counters.  The environment reports the
// per-test and per-suite deltas, and the run's total, as test properties.
struct Statistics {
  int skipped_tests = 0;
  int listed_skips = 0;
  int tolerated_failures = 0;
  int expected_failures = 0;
  int unexpected_failures = 0;
  int expected_successes = 0;
  int unexpected_successes = 0;

  static Statistics From(const TestManager& manager);
  Statistics operator-(const Statistics& other) const;
  Statistics& operator+=(const Statistics& other);

  // Records each statistic as a gtest property of the current test (or suite,
  // or run) under the names skipped_tests, listed_skips,
  // tolerated_failures, expected_failures, unexpected_failures,
  // expected_successes and unexpected_successes.
  void RecordProperties() const;
};

// Test helpers, defined in test_environment_testing.h.
class ScopedGlobalConformanceEnvironment;
class ScopedPartialRunOverride;

// The process-global state of a conformance test binary.  See the file comment
// for an overview.
//
// This is a testing::Environment, so SetUp() and TearDown() are genuine
// overrides that gtest runs around the tests.  It can't be handed to
// testing::AddGlobalTestEnvironment() directly, and its constructor is private
// to make sure of that.  gtest deletes the environments registered with it at
// the end of RUN_ALL_TESTS() (see RunAllTests in gtest.cc).  The installed
// instance must outlive that, because the transitional merged runner
// (conformance_test_main.cc) reads test_manager() afterwards.  Install()
// therefore registers a private proxy with gtest that forwards to SetUp() and
// TearDown().  The environment object itself is intentionally leaked.  The
// testee is not: TearDown() releases it.
// TODO: b/563707827 - once the merged runner has retired (its users moved to
// the conformance_test() macro), nothing needs the environment after
// RUN_ALL_TESTS(); register it directly with AddGlobalTestEnvironment() and
// delete the proxy.
//
// Every conformance test a gtest test runs must be checked with Yields() (see
// matchers.h), which records its outcome as a gtest success in the running
// test.  A listener the environment registers with gtest sees the success at
// once and tallies the outcome in the TestManager.  At the end of each gtest
// test (and test suite) the listener records the outcomes checked in it as
// properties named after the conformance tests (see ReportRecordedResults())
// and the statistics the test added as its properties.
//
// Single-threaded.  See the file comment.
class ConformanceEnvironment : public testing::Environment {
 public:
  ~ConformanceEnvironment() override;

  ConformanceEnvironment(const ConformanceEnvironment&) = delete;
  ConformanceEnvironment& operator=(const ConformanceEnvironment&) = delete;

  // Creates an environment, hooks its SetUp() and TearDown() into gtest and
  // makes it the process-global instance.  Must be called exactly once, before
  // RUN_ALL_TESTS():
  //
  //   ConformanceEnvironment::Install(OptionsFromFlags());
  //   return RUN_ALL_TESTS();
  //
  // The environment itself is never destroyed (see the class comment), but
  // TearDown() releases its testee.
  static ConformanceEnvironment& Install(ConformanceEnvironmentOptions options);

  // Returns the process-global instance.  Check-fails if Install() hasn't been
  // called, which typically means test_environment_main wasn't linked in.
  static ConformanceEnvironment& Get();

  // Loads the failure lists.  Any failure here is fatal, so no tests run.
  // gtest runs this before the first test (see Install()).
  void SetUp() override;

  // Finishes the run.  gtest runs this after the last test (see Install()).
  //
  // Reports the results checked outside any test suite (see
  // ReportRecordedResults()) and records the run's statistics as test
  // properties.  Fails for every failure list entry that matched too many
  // tests (see TestManager::OverexpandedWildcards()).  Rewrites the failure
  // list if `fix` was requested and all of the tests ran.  Fails if expected
  // failures were never seen, unless `check_unseen_expected_failures` is false
  // or only a subset of the tests ran (see `fix`).  Finally releases
  // the testee.  An owned runner is destroyed, which for a ForkPipeRunner
  // stops the process.  For a caller-owned runner only our reference is
  // dropped, so the caller may destroy the runner right after
  // RUN_ALL_TESTS().  The merged runner relies on this.
  //
  // Also safe to call if SetUp() never ran because gtest selected no test.  It
  // then records empty statistics, skips --fix and the unseen check, and
  // releases the testee.  conformance_test_main relies on this.
  void TearDown() override;

  // Records the outcome of every conformance test checked since the last call
  // as a property of the current gtest test (or suite, or run) named after the
  // conformance test, with ResultRecord::ToString() as the value, so that the
  // reports can tell what each test did.  The outcomes themselves were tallied
  // in the TestManager as they were checked.  Also fails the current test for
  // every gtest success that claimed to carry an outcome but couldn't be
  // decoded (see ParseResultRecordMessage() in result_record.h), so that no
  // outcome goes missing quietly.  The environment's listener calls it when a
  // test or a suite ends, and TearDown() for anything checked outside a test
  // suite.
  void ReportRecordedResults();

  // Where the tests' outcomes are recorded.  The transitional merged runner
  // (conformance_test_main.cc) reads it after RUN_ALL_TESTS().
  const TestManager& test_manager() const { return test_manager_; }

  // The connection to the testee.  Check-fails once TearDown() has released
  // it.
  Testee& testee();

  const ConformanceEnvironmentOptions& options() const { return options_; }

 private:
  friend class ScopedGlobalConformanceEnvironment;
  friend class ScopedPartialRunOverride;
  class ResultListener;

  // Creates an environment and makes it the process-global one.  Check-fails if
  // there already is one.  The testee connection is created eagerly, though a
  // ForkPipeRunner only spawns the testee on first use.  Failure lists are
  // loaded in SetUp().
  explicit ConformanceEnvironment(ConformanceEnvironmentOptions options);

  ConformanceEnvironmentOptions options_;

  // The runner we own, from `owned_runner` or spawned for `testee_binary`.
  // Null when using the caller's `runner`.  Released in TearDown().
  std::unique_ptr<ConformanceTestRunner> owned_runner_;

  TestManager test_manager_;

  // Null once TearDown() has released it.
  std::unique_ptr<Testee> testee_;

  // What TearDown() takes the gtest run to be, instead of asking gtest whether
  // only some of the tests ran.  Set only by ScopedPartialRunOverride.
  absl::optional<bool> partial_run_override_;

  bool set_up_succeeded_ = false;
  // Tallies the outcomes Yields() records as gtest reports them, and reports
  // them and the statistics at the end of every gtest test and suite.  gtest
  // owns it while it is registered; the destructor takes it back.
  testing::TestEventListener* absl_nonnull result_listener_;

  // The outcomes checked since ReportRecordedResults() last ran, in order, and
  // the gtest successes that claimed to carry an outcome but couldn't be
  // decoded.  The listener fills both in.
  std::vector<RecordedResult> recorded_results_;
  std::vector<std::string> undecodable_results_;
};

}  // namespace internal
}  // namespace conformance
}  // namespace protobuf
}  // namespace google

#endif  // GOOGLE_PROTOBUF_CONFORMANCE_TEST_ENVIRONMENT_H__
