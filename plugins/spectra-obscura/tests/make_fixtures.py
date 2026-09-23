"""Generates the parity fixtures for obscura-tests from Obscura's own code.

    python make_fixtures.py <obscura checkout> <out dir>

Needs numpy, scipy, scikit-learn, cryptography (Obscura's requirements).
"""
import json
import random
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, sys.argv[1])
out = Path(sys.argv[2])

from obscura import crypto  # noqa: E402
from obscura.classifier import Learner  # noqa: E402
from obscura.definitions import Definitions, _canonical, write_obx  # noqa: E402
from sklearn.feature_extraction.text import TfidfVectorizer  # noqa: E402

rng = random.Random(7)
channels = {"admin chat": 1, "support chat": 1, "report / question": 1, "ooc": 0, "emote": 0, "law": 0,
            "say": 0, "whisper": 0, "dispatch": None, "faction governor": 0}
words = ("can you see the fire copy that report accepted player car stolen help please "
         "ticket staff ban kick weapon pistol radio dispatch unit Émile café 10-4 mdt").split()


def sample():
    ch = rng.choice(list(channels))
    body = " ".join(rng.choice(words) for _ in range(rng.randint(1, 9)))
    label = channels[ch]
    if label is None:
        label = int("staff" in body or "ban" in body)
    colour = [round(rng.random(), 4) for _ in range(13)] if rng.random() > 0.1 else None
    return {"channel": ch, "tags": [ch.split()[0]], "body": body, "colour": colour}, label


train = [sample() for _ in range(160)]
test = [sample()[0] for _ in range(40)]
test.append({"channel": "admin chta", "tags": [], "body": "typo channel", "colour": None})
test.append({"channel": "brand new", "tags": [], "body": "never seen", "colour": [0.0] * 13})

with tempfile.TemporaryDirectory() as tmp:
    learner = Learner(Path(tmp), ["admin chat", "report"])
    empty = [p.prob for p in learner.predict(test)]
    for i in range(0, len(train), 8):
        chunk = train[i:i + 8]
        learner.add_screen([f for f, _ in chunk], [bool(l) for _, l in chunk])
    probs = [p.prob for p in learner.predict(test)]
    sources = [p.source for p in learner.predict(test)]
    jsonl = (Path(tmp) / "training.jsonl").read_text(encoding="utf-8")

# The same learner solved to a tight tolerance: the C++ L-BFGS converges fully,
# sklearn's lbfgs stops at tol=1e-4
import sklearn.linear_model as _lm  # noqa: E402
import obscura.classifier as _oc  # noqa: E402


class _Tight(_lm.LogisticRegression):
    def __init__(self, **kw):
        kw.update(tol=1e-12, max_iter=100000)
        super().__init__(**kw)


_oc.LogisticRegression = _Tight
with tempfile.TemporaryDirectory() as tmp:
    tight = Learner(Path(tmp), ["admin chat", "report"])
    for i in range(0, len(train), 8):
        chunk = train[i:i + 8]
        tight.add_screen([f for f, _ in chunk], [bool(l) for _, l in chunk])
    probs_tight = [p.prob for p in tight.predict(test)]

docs = ["Admin Chat | Admin Chat | Can you  see\tthe fire?", "a", "ab", "Émile café 10-4"]
vec = TfidfVectorizer(analyzer="char_wb", ngram_range=(2, 4), sublinear_tf=True).fit(docs)
ngrams = [vec.build_analyzer()(d) for d in docs]
matrix = vec.transform(docs).toarray()
tfidf = [{g: round(float(matrix[r][i]), 12) for g, i in vec.vocabulary_.items() if matrix[r][i]} for r in range(len(docs))]

pem, pub = crypto.generate_keypair()
defs = Definitions(defs_version=3, channel_stats={"admin chat": [0, 5], "émote": [7, 0], "z": [1, 1]},
                   seeds=["report", "admin chat"], created="2026-09-22T12:00:00+00:00", app_version="0.4.0")
write_obx(out / "test-defs.obx", defs, pem)
payload = defs.payload()

json.dump({
    "train": [{**f, "label": l} for f, l in train], "test": test,
    "empty_probs": empty, "probs": probs, "probs_tight": probs_tight, "sources": sources,
    "docs": docs, "ngrams": ngrams, "tfidf": tfidf,
    "test_public_key": pub, "test_private_pem": pem,
    "payload_canonical": _canonical(payload).decode("ascii"),
    "payload_signature": crypto.sign(pem, _canonical(payload)),
}, (out / "fixtures.json").open("w", encoding="utf-8"), ensure_ascii=False, indent=1)
(out / "training.jsonl").write_text(jsonl, encoding="utf-8")
print("ok", len(train), "train", len(test), "test")
