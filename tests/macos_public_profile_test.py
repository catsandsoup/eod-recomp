"""Run normal profile selection with the metadata of a public macOS release."""
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    contents = root / "EOD.app" / "Contents"
    executable = contents / "MacOS" / "profile-test"
    executable.parent.mkdir(parents=True)
    shutil.copy2(sys.argv[1], executable)
    (contents / "Info.plist").write_bytes(plistlib.dumps({
        "CFBundleExecutable": executable.name,
        "CFBundleIdentifier": "local.eod.test.public-profile",
        "CFBundlePackageType": "APPL",
        "EODIntegratedContentOptional": True,
    }))
    subprocess.run([executable, root / "support", "--public-release"], check=True)
