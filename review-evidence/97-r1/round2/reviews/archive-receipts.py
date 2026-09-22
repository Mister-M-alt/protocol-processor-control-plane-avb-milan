"""Copy completed factual receipts into the public evidence checkout.

This does not commit, push, or interpret test results. Pass source=relative-target
for each completed file/directory. Review every diff before publishing.
"""
from pathlib import Path
import hashlib
import json
import sys

ROOT = Path('$VALIDATION_STORAGE/lanes/97-review-evidence/review-evidence/97-r1')
MANIFEST = ROOT / 'MANIFEST.json'
REPLACEMENTS = (
    ('$WORKSPACE_HOME', '$WORKSPACE_HOME'),
    ('$CANDIDATE', '$CANDIDATE'),
    ('$VALIDATION', '$VALIDATION'),
    ('$MUTATION_STORAGE', '$MUTATION_STORAGE'),
    ('$BUILDER_VALIDATION', '$BUILDER_VALIDATION'),
    ('$VALIDATION', '$VALIDATION'),
    ('$AC5_CANDIDATE', '$AC5_CANDIDATE'),
    ('$TRUSTED_DEV', '$TRUSTED_DEV'),
    ('$VALIDATION_STORAGE', '$VALIDATION_STORAGE'),
    ('$VALIDATION_TOOLS', '$VALIDATION_TOOLS'),
    ('$VALIDATION_STORAGE', '$VALIDATION_STORAGE'),
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


records = {r['file']: r for r in json.loads(MANIFEST.read_text())}
for argument in sys.argv[1:]:
    source_name, target_name = argument.split('=', 1)
    source = Path(source_name)
    target = ROOT / target_name
    assert target.resolve().is_relative_to(ROOT.resolve())
    assert source.exists(), source
    paths = sorted(p for p in source.rglob('*') if p.is_file()) if source.is_dir() else [source]
    for path in paths:
        if "__pycache__" in path.parts or path.suffix == ".pyc":
            continue
        original = path.read_bytes()
        try:
            original.decode('utf-8')
        except UnicodeDecodeError:
            # R230 factual diagnostics deliberately exercise legacy encodings.
            # Preserve their bytes; neutralize only the same ASCII path prefixes.
            assert path.name in {
                '13-prior-DE_LATIN1.log', '13-base-make-ltn_rom-JA_EUCJP.log',
                '13-prior-JA_EUCJP.log', '13-prior-FR_LATIN9.log',
                '13-head-JA_EUCJP.log',
            }, path
        published = original
        for before, after in REPLACEMENTS:
            published = published.replace(before.encode('ascii'), after.encode('ascii'))
        destination = target / path.relative_to(source) if source.is_dir() else target
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(published)
        relative = str(destination.relative_to(ROOT))
        records[relative] = {
            'file': relative,
            'original_sha256': digest(original),
            'published_sha256': digest(published),
            'path_redacted': original != published,
        }
        print(relative, len(published), records[relative]['published_sha256'])
MANIFEST.write_text(json.dumps(list(records.values()), indent=2) + '\n')
