import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# A stand-in for playprobe: it records its arguments and ends a quest run as playprobe does.
FAKE_PROBE = """#!/bin/sh
echo "$@" >> "$(dirname "$0")/calls.txt"
echo "QUEST SUMMARY: 1 passed, 0 failed, 1 of 1 steps run"
exit 0
"""


class LiveSmokeTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        probe = self.root / "playprobe"
        probe.write_text(FAKE_PROBE)
        probe.chmod(0o755)
        self.plain = self.root / "plain.quest"
        self.plain.write_text("# a script\nwait 1\n")
        self.caster = self.root / "spells.quest"
        self.caster.write_text("# a script\n# live-smoke: caster\nwait 1\n")

    def tearDown(self):
        self.dir.cleanup()

    def run_smoke(self, *scripts, caster=None):
        env = dict(os.environ, PLAYPROBE=str(self.root / "playprobe"), EQ_GAME_DIR=str(self.root),
                   LIVE_SMOKE_LOGS=str(self.root / "logs"), LIVE_SMOKE_PAUSE="0")
        env.pop("LIVE_SMOKE_CASTER", None)
        if caster:
            env["LIVE_SMOKE_CASTER"] = caster
        r = subprocess.run(["bash", str(ROOT / "tools/live_smoke.sh"), "host:1", "user", "pass", "Warrior",
                            *map(str, scripts)], env=env, capture_output=True, text=True, check=False)
        calls = self.root / "calls.txt"
        return r, calls.read_text().splitlines() if calls.exists() else []

    def test_a_plain_script_plays_as_the_character_with_a_time_limit(self):
        r, calls = self.run_smoke(self.plain)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("PASS plain", r.stdout)
        self.assertEqual(len(calls), 1)
        self.assertIn("--char Warrior", calls[0])
        self.assertIn("--seconds 480", calls[0])

    def test_a_caster_script_is_skipped_without_a_caster(self):
        r, calls = self.run_smoke(self.plain, self.caster)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("SKIP spells", r.stdout)
        self.assertEqual(len(calls), 1)

    def test_a_caster_script_plays_as_the_caster(self):
        r, calls = self.run_smoke(self.plain, self.caster, caster="Cleric")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("PASS spells", r.stdout)
        self.assertEqual(len(calls), 2)
        self.assertIn("--char Warrior", calls[0])
        self.assertIn("--char Cleric", calls[1])

    def test_a_failed_script_fails_the_run(self):
        (self.root / "playprobe").write_text(FAKE_PROBE.replace("0 failed", "1 failed").replace("exit 0", "exit 1"))
        r, _ = self.run_smoke(self.plain)
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAIL plain", r.stdout)


if __name__ == "__main__":
    unittest.main()
