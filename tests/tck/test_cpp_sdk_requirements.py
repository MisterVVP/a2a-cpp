"""Additional executable TCK requirements for the C++ SDK.

The mandatory runner stages this module beside the unmodified upstream tests,
so the upstream collector and report format include real C++ SDK assertions.
"""

import os
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

import pytest

from tck.requirements.registry import get_requirement_by_id


BINDINGS = {"http_json": "HttpJson", "jsonrpc": "JsonRpc", "grpc": "Grpc"}
POSITIVE = (
    "CrossBindingTaskHistoryArtifactsAndCancellation",
    "DirectMessagesHaveEquivalentContent",
    "PushConfigurationIsSharedAcrossBindings",
    "LiveSubscriptionPreservesTaskAcrossBindings",
    "StreamingResultsAreEquivalent",
    "ListTasksProducesEquivalentPages",
)
NEGATIVE = ("NegativePushProfileHasPrescribedBindingError", "NegativeStreamingProfileHasPrescribedBindingError")
AUTH = ("AuthenticationRulesSurviveCrossBindingTaskAccess",)
LIVE_VERSION = ("RequestedVersionsUseSdkNegotiation",)
DEFAULT_VERSION = ("EveryClientOperationSendsDefaultVersion",)
PATCH_VERSION = ("RequestedPatchIsNormalizedAndHeaderAliasesAreRemoved", "ServerIgnoresWirePatchBeforeDispatch")
REJECT_VERSION = ("UnsupportedMinorAndEmptyFallbackNeverDispatch", "UnsupportedVersionUsesPrescribedBindingCode")
# Each tuple is (profile, scenario names). "adapter" constructs the SDK directly,
# independently of SUT implementation; other profiles launch the real TCK SUT.
SCENARIOS = {
    "BIND-EQUIV-001": (("default", POSITIVE), ("capability_negative", NEGATIVE)),
    "BIND-EQUIV-002": (("default", POSITIVE),),
    "BIND-EQUIV-003": (("default", ("MissingTaskHasPrescribedBindingError",
                                     "TerminalTaskSubscriptionUsesEquivalentError")),
                       ("capability_negative", NEGATIVE), ("default", LIVE_VERSION)),
    "BIND-EQUIV-004": (("authenticated", AUTH),),
    "VER-CLIENT-001": (("adapter", DEFAULT_VERSION),),
    "VER-CLIENT-002": (("adapter", PATCH_VERSION),),
    "VER-SERVER-001": (("adapter", DEFAULT_VERSION + PATCH_VERSION), ("default", LIVE_VERSION)),
    "VER-SERVER-002": (("adapter", REJECT_VERSION), ("default", LIVE_VERSION)),
    "VER-SERVER-003": (("adapter", (REJECT_VERSION[0],)), ("default", LIVE_VERSION)),
    "CORE-CAP-001": (("capability_negative", (NEGATIVE[0],)),),
    "CORE-CAP-002": (("capability_negative", (NEGATIVE[1],)),),
}


def run_scenarios(requirement_id, transport, profile, names, artifact_dir):
    root = Path(os.environ["A2A_CPP_SDK_ROOT"])
    build = Path(os.environ["A2A_CPP_SDK_BUILD"])
    backend = os.environ["A2A_TCK_STORE_BACKEND"]
    binding = BINDINGS[transport]
    artifact_dir.mkdir(parents=True, exist_ok=True)
    if profile == "adapter":
        report = artifact_dir / "adapter.xml"
        pattern = ":".join(f"*.{name}/{binding}" for name in names)
        command = [str(build / "tests/protocol_version_integration_test"),
                   f"--gtest_filter={pattern}", f"--gtest_output=xml:{report}"]
        expected_count = len(names)
    else:
        report = artifact_dir / f"{profile}.xml"
        # Each target binding must interoperate with all three source bindings.
        pattern = ":".join(f"*.{name}/*To{binding}" for name in names)
        command = [sys.executable, str(root / "tests/scripts/run_binding_scenarios.py"),
                   "--sut", str(build / "tests/tck_sut"),
                   "--scenarios", str(build / "tests/shared_binding_test"),
                   "--backend", backend, "--profile", profile, "--gtest-filter", pattern,
                   "--report-dir", str(artifact_dir), "--require-postgres"]
        expected_count = len(names) * len(BINDINGS)
    report.unlink(missing_ok=True)
    result = subprocess.run(command, capture_output=True, text=True, timeout=180, check=False)
    (artifact_dir / "scenarios.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode == 0, f"{requirement_id} {transport} {profile}:\n{result.stdout}\n{result.stderr}"
    cases = list(ET.parse(report).getroot().iter("testcase"))
    assert len(cases) == expected_count, f"Expected {expected_count} SDK scenarios; got {len(cases)}"
    for case in cases:
        assert case.get("status") == "run" and case.get("result") == "completed", case.attrib
        assert all(case.find(tag) is None for tag in ("skipped", "failure", "error")), ET.tostring(case)


@pytest.mark.must
@pytest.mark.parametrize("transport", tuple(BINDINGS))
@pytest.mark.parametrize("requirement_id", tuple(SCENARIOS))
def test_cpp_sdk_requirement(requirement_id, transport, compatibility_collector):
    """Execute assertions, record actual outcomes, and prohibit missing/skip coverage."""
    req = get_requirement_by_id(requirement_id)
    errors = []
    try:
        artifacts = Path(os.environ["A2A_CPP_SDK_ARTIFACTS"]) / requirement_id / transport
        for index, (profile, names) in enumerate(SCENARIOS[requirement_id]):
            run_scenarios(requirement_id, transport, profile, names, artifacts / str(index))
    except Exception as error:
        errors.append(str(error))
    compatibility_collector.record(requirement_id=req.id, transport=transport,
                                   level=req.level.value, passed=not errors, errors=errors)
    assert not errors, "\n".join(errors)
