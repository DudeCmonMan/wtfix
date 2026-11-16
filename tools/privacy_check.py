"""Privacy and release checks for wartales-prod: personal data, local paths, secrets, debug features and build
artifacts in files, and commit identities and trailers. Personal terms come from .git/info/privacy-denylist (one per
line, never committed). Exit 1 on any finding.
Usage: python -I tools/privacy_check.py --staged | --msg <commit message file> | --all
"""
import os
import re
import subprocess
import sys

NOREPLY = re.compile(r"^\d+\+[A-Za-z0-9-]+@users\.noreply\.github\.com$")
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}")
CONTENT = [
    ("local path", re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:[\\/][A-Za-z]|[\\/]Users[\\/]|/home/[a-z]|AppData")),
    ("Steam ID", re.compile(r"\b7656119\d{10}\b")),
    ("IP address", re.compile(r"\b(?!127\.0\.0\.1\b|0\.0\.0\.0\b)(?:\d{1,3}\.){3}\d{1,3}\b")),
    ("secret", re.compile(r"(?i)\b(?:token|secret|password|passwd|api[_-]?key)\b\s*[:=]\s*[\"']?[\w\-./+]{12,}"
                          r"|-----BEGIN [A-Z ]*PRIVATE KEY-----|\bgh[pousr]_[A-Za-z0-9]{36,}\b|\bAKIA[0-9A-Z]{16}\b")),
    ("debug or private-repo reference", re.compile(
        r"(?i)wtremote|dump_patched|selftest|wtfix_\w*_test|smoke_?api|koaloader|\[dev\]"
        r"|wartales-dev|wartales-disassembly|code-projects|captures/|docs/\w+\.md|todo\.md")),
]
TRAILER = re.compile(r"(?im)^\s*(?:co-authored-by|signed-off-by)\s*:|generated with|\U0001F916")
ARTIFACT = re.compile(r"(?i)\.(?:dll|hdll|exe|lib|obj|o|a|pdb|exp|zip|dat|pak|sav|log|dmp|bin)$"
                      r"|^(?:build|dist|sdk|captures|docs|save)/")
SELF = "tools/privacy_check.py"


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, check=True).stdout


def denylist():
    path = os.path.join(git("rev-parse", "--git-common-dir").decode().strip(), "info", "privacy-denylist")
    if not os.path.exists(path):
        return []
    terms = [t.strip() for t in open(path, encoding="utf-8") if t.strip() and not t.startswith("#")]
    return [re.compile(r"(?i)(?<![A-Za-z0-9])" + re.escape(t) + r"(?![A-Za-z0-9])") for t in terms]


def scan_text(where, text, deny, problems, third_party=False, self_file=False):
    for i, line in enumerate(text.splitlines(), 1):
        for d in deny:
            if d.search(line):
                problems.append("%s:%d: a term from the private deny-list" % (where, i))
        if self_file:
            continue
        for m in EMAIL.finditer(line):
            if not NOREPLY.match(m.group()) and not third_party:
                problems.append("%s:%d: email address %s" % (where, i, m.group()))
        # Vendored code stays as released: only personal terms and secrets are ours to catch there.
        for name, rx in CONTENT:
            if rx.search(line) and not (third_party and name != "secret"):
                problems.append("%s:%d: %s: %s" % (where, i, name, line.strip()[:100]))


def scan_file(path, data, deny, problems):
    if ARTIFACT.search(path):
        problems.append("%s: build artifact or private file" % path)
    for d in deny:
        if d.search(path):
            problems.append("%s: a term from the private deny-list in the name" % path)
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        problems.append("%s: binary file" % path)
        return
    scan_text(path, text, deny, problems, path.startswith("third_party/"), path == SELF)


def check_ident(what, ident, deny, problems):
    email = ident.rsplit("<", 1)[-1].split(">", 1)[0]
    if not NOREPLY.match(email):
        problems.append("%s email is not a GitHub noreply address" % what)
    for d in deny:
        if d.search(ident):
            problems.append("%s has a term from the private deny-list" % what)


def check_message(where, message, deny, problems):
    if TRAILER.search(message):
        problems.append("%s: attribution trailer or footer" % where)
    scan_text(where, message, deny, problems)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("--staged", "--msg", "--all"):
        print(__doc__.strip().splitlines()[-1])
        return 2
    deny, problems = denylist(), []
    mode = sys.argv[1]
    if mode == "--msg":
        check_message("commit message", open(sys.argv[2], encoding="utf-8").read(), deny, problems)
    elif mode == "--staged":
        for path in git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z").decode().split("\0"):
            if path:
                scan_file(path, git("show", ":" + path), deny, problems)
        for var in ("GIT_AUTHOR_IDENT", "GIT_COMMITTER_IDENT"):
            check_ident(var.split("_")[1].lower(), git("var", var).decode(), deny, problems)
    else:
        for path in git("ls-files", "-z").decode().split("\0"):
            if path:
                scan_file(path, open(path, "rb").read(), deny, problems)
        log = git("log", "--all", "--format=%H%x00%an <%ae>%x00%cn <%ce>%x00%B%x1e").decode()
        for entry in filter(str.strip, log.split("\x1e")):
            sha, author, committer, message = entry.strip("\n").split("\0", 3)
            check_ident("commit %s author" % sha[:8], author, deny, problems)
            check_ident("commit %s committer" % sha[:8], committer, deny, problems)
            check_message("commit %s message" % sha[:8], message, deny, problems)
    for p in problems:
        print("privacy: " + p)
    if problems:
        print("privacy: %d problem(s); nothing may be committed or published until they are gone" % len(problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
