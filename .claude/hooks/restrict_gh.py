import json
import sys

data = json.load(sys.stdin)
cmd = data.get("tool_input", {}).get("command", "")

ok = True
if "gh " in cmd:
    if not cmd.startswith("gh pr create"):
        print(
            json.dumps({"decision": "block", "reason": "Only gh pr create is allowed"})
        )
        exit(0)
    if "--repo" in cmd or "-R" in cmd:
        print(
            json.dumps(
                {"decision": "block", "reason": "Only pr to default repo is allowed"}
            )
        )
        exit(0)
print(json.dumps({"decision": "approve"}))
