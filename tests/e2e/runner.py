#!/usr/bin/env python3
"""
tests/e2e/runner.py - Central Automated Test Runner for ref_openremix (Tiers 1-4)
Executes end-to-end tests across all four tiers:
  - Tier 1: Feature Coverage (Features 1-18, >= 5 tests each, 90 tests)
  - Tier 2: Boundary & Corner Cases (Features 1-18, >= 5 tests each, 90 tests)
  - Tier 3: Cross-Feature Combinations (Pairwise interactions, 15 tests)
  - Tier 4: Real-World Scenarios (Map loads, combat stress, level transitions, 7 tests)

Total: 202 comprehensive test cases.
Returns standard exit code 0 on all tests passing, non-zero on failure.
"""

import sys
import os
import time
import argparse
import unittest
from pathlib import Path

# Ensure project root is in sys.path
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tests.e2e import test_tier1_features
from tests.e2e import test_tier2_boundaries
from tests.e2e import test_tier3_combinations
from tests.e2e import test_tier4_scenarios


TIER_MODULES = {
    1: ("Tier 1: Feature Coverage", test_tier1_features),
    2: ("Tier 2: Boundary & Corner Cases", test_tier2_boundaries),
    3: ("Tier 3: Cross-Feature Combinations", test_tier3_combinations),
    4: ("Tier 4: Real-World Scenarios", test_tier4_scenarios),
}


def build_suite(selected_tier=None, feature_filter=None):
    """Builds unittest.TestSuite based on tier and feature filters."""
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()

    tiers_to_run = [selected_tier] if selected_tier in TIER_MODULES else sorted(TIER_MODULES.keys())

    for tier_num in tiers_to_run:
        tier_name, module = TIER_MODULES[tier_num]
        tier_suite = loader.loadTestsFromModule(module)

        if feature_filter is not None and tier_num in (1, 2):
            feat_tag = f"f{feature_filter:02d}"
            feat_tag_b = f"b{feature_filter:02d}"
            filtered_suite = unittest.TestSuite()
            for test in iter_tests(tier_suite):
                test_name = test._testMethodName.lower()
                if feat_tag in test_name or feat_tag_b in test_name:
                    filtered_suite.addTest(test)
            suite.addTest(filtered_suite)
        else:
            suite.addTest(tier_suite)

    return suite


def iter_tests(suite):
    """Recursively yields individual test cases from a TestSuite."""
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from iter_tests(item)
        else:
            yield item


def run_tests(tier="all", feature=None, verbose=False):
    """Executes the test suite and prints structured reporting."""
    selected_tier = int(tier) if tier in ("1", "2", "3", "4") else None
    suite = build_suite(selected_tier, feature)

    total_tests = suite.countTestCases()
    print("=" * 80)
    print("  ref_openremix E2E Automated Test Runner (Tiers 1-4)")
    print("=" * 80)
    print(f"Target Project:  {PROJECT_ROOT}")
    print(f"Selected Scope:  Tier {tier if selected_tier else '1-4 (All)'}")
    if feature:
        print(f"Feature Filter:  Feature {feature:02d}")
    print(f"Total Discovered Tests: {total_tests}")
    print("-" * 80)

    start_time = time.perf_counter()
    runner = unittest.TextTestRunner(
        verbosity=2 if verbose else 1,
        stream=sys.stdout,
        resultclass=unittest.TextTestResult,
    )
    result = runner.run(suite)
    elapsed = time.perf_counter() - start_time

    print("\n" + "=" * 80)
    print("  E2E Test Execution Summary")
    print("=" * 80)
    print(f"  Total Executed: {result.testsRun}")
    print(f"  Passed:         {result.testsRun - len(result.failures) - len(result.errors)}")
    print(f"  Failures:       {len(result.failures)}")
    print(f"  Errors:         {len(result.errors)}")
    print(f"  Skipped:        {len(result.skipped)}")
    print(f"  Elapsed Time:   {elapsed:.3f} seconds")
    print("-" * 80)

    if result.wasSuccessful():
        print("  STATUS: [PASS] All tests passed specification contracts successfully.")
        print("=" * 80)
        return 0
    else:
        print("  STATUS: [FAIL] One or more tests failed.")
        if result.failures:
            print("\nFailures:")
            for test, err in result.failures:
                print(f"  - {test.id()}: {err.strip().splitlines()[-1]}")
        if result.errors:
            print("\nErrors:")
            for test, err in result.errors:
                print(f"  - {test.id()}: {err.strip().splitlines()[-1]}")
        print("=" * 80)
        return 1


def main():
    parser = argparse.ArgumentParser(description="ref_openremix E2E Test Runner")
    parser.add_argument(
        "--tier",
        choices=["1", "2", "3", "4", "all"],
        default="all",
        help="Test tier to execute (default: all)",
    )
    parser.add_argument(
        "--feature",
        type=int,
        choices=range(1, 19),
        metavar="[1-18]",
        default=None,
        help="Filter Tier 1/2 tests by feature number (1-18)",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Enable detailed test case output",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List all discovered tests without running them",
    )

    args = parser.parse_args()

    if args.list:
        selected_tier = int(args.tier) if args.tier in ("1", "2", "3", "4") else None
        suite = build_suite(selected_tier, args.feature)
        print(f"Listing {suite.countTestCases()} tests:")
        for idx, test in enumerate(iter_tests(suite), 1):
            print(f"  {idx:3d}. {test.id()}")
        sys.exit(0)

    exit_code = run_tests(tier=args.tier, feature=args.feature, verbose=args.verbose)
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
