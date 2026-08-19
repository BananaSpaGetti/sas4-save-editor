"""
Train a detector that tells a consistent SAS4 profile from a tampered one, and show what it
learned. This is the blue side of the red/blue exercise done with machine learning: the
model is a classifier over the features in `sas4_model.py`, and the point is to study the
boundary between edited and legitimate, not to defeat anything.

It carries its own decision tree in pure Python, so it runs on this machine with nothing
installed -- and a from-scratch tree is more use for learning than a black box, because the
rules it splits on are printed and readable. If scikit-learn is present it also trains one
of those and reports its score, for comparison.

    py sas4_train.py                       generate data, train, evaluate, print the tree
    py sas4_train.py --count 4000 --depth 5
    py sas4_train.py --data decoded/train.jsonl     train on a dataset already written

What to look for in the output: the tree reaches the low-to-mid 0.9s and no further, and the
confusion matrix says why -- the errors are tampered profiles it calls clean. Those are the
internally-consistent edits (a level raised with its XP and rank to match), which no feature
computed from a single file can separate from a real one. That ceiling is the lesson: a
detector that sees one save at a time cannot do what a server does with an account's history
and the shape of millions of others.
"""

import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("sas4_model", os.path.join(HERE, "sas4_model.py"))
model = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(model)

FEATURES = model.FEATURE_NAMES


# --- data --------------------------------------------------------------------------------

def make_rows(count, seed):
    """Labelled feature dicts, the same way `sas4_model.py dataset` builds them."""
    import random
    rng = random.Random(seed)
    rows = []
    for _ in range(count):
        document = model.generate(level=rng.randint(1, 25), money=rng.randint(0, 2_000_000))
        label = 0 if rng.random() < 0.5 else 1
        if label:
            model.tamper(document, rng)
        row = model.features(document)
        row["label"] = label
        rows.append(row)
    return rows


def load_rows(path):
    if path.endswith(".csv"):
        import csv
        with open(path, encoding="utf-8") as handle:
            return [{k: int(float(v)) for k, v in r.items()} for r in csv.DictReader(handle)]
    with open(path, encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


def split(rows, fraction, seed):
    import random
    shuffled = list(rows)
    random.Random(seed).shuffle(shuffled)
    cut = int(len(shuffled) * fraction)
    return shuffled[cut:], shuffled[:cut]         # train, test


# --- a decision tree, from scratch -------------------------------------------------------

class Leaf:
    def __init__(self, label, n0, n1):
        self.label, self.n0, self.n1 = label, n0, n1


class Node:
    def __init__(self, feature, threshold, left, right):
        self.feature, self.threshold = feature, threshold
        self.left, self.right = left, right      # left: feature <= threshold


def _gini(rows):
    if not rows:
        return 0.0
    ones = sum(r["label"] for r in rows)
    p = ones / len(rows)
    return 1 - p * p - (1 - p) * (1 - p)


def _best_split(rows):
    """The (feature, threshold) that most reduces Gini impurity, or None if none helps."""
    base = _gini(rows)
    best = None
    best_gain = 1e-9
    for feature in FEATURES:
        values = sorted({r[feature] for r in rows})
        for a, b in zip(values, values[1:]):
            threshold = (a + b) / 2
            left = [r for r in rows if r[feature] <= threshold]
            right = [r for r in rows if r[feature] > threshold]
            if not left or not right:
                continue
            weighted = (len(left) * _gini(left) + len(right) * _gini(right)) / len(rows)
            gain = base - weighted
            if gain > best_gain:
                best_gain = gain
                best = (feature, threshold)
    return best


def build_tree(rows, depth, max_depth):
    ones = sum(r["label"] for r in rows)
    zeros = len(rows) - ones
    if depth >= max_depth or ones == 0 or zeros == 0:
        return Leaf(1 if ones >= zeros else 0, zeros, ones)
    chosen = _best_split(rows)
    if not chosen:
        return Leaf(1 if ones >= zeros else 0, zeros, ones)
    feature, threshold = chosen
    left = [r for r in rows if r[feature] <= threshold]
    right = [r for r in rows if r[feature] > threshold]
    return Node(feature, threshold,
                build_tree(left, depth + 1, max_depth),
                build_tree(right, depth + 1, max_depth))


def classify(tree, row):
    while isinstance(tree, Node):
        tree = tree.left if row[tree.feature] <= tree.threshold else tree.right
    return tree.label


def print_tree(tree, indent="  "):
    if isinstance(tree, Leaf):
        name = "tampered" if tree.label else "consistent"
        print("%s-> %s  (%d consistent, %d tampered in training)" % (indent, name, tree.n0, tree.n1))
        return
    print("%sif %s <= %g:" % (indent, tree.feature, tree.threshold))
    print_tree(tree.left, indent + "    ")
    print("%selse:" % indent)
    print_tree(tree.right, indent + "    ")


# --- evaluation --------------------------------------------------------------------------

def evaluate(name, predict, test):
    tp = fp = tn = fn = 0
    for row in test:
        pred, truth = predict(row), row["label"]
        if truth == 1 and pred == 1:
            tp += 1
        elif truth == 1 and pred == 0:
            fn += 1
        elif truth == 0 and pred == 1:
            fp += 1
        else:
            tn += 1
    total = len(test)
    accuracy = (tp + tn) / total if total else 0
    precision = tp / (tp + fp) if tp + fp else 0
    recall = tp / (tp + fn) if tp + fn else 0
    print("\n%s" % name)
    print("  accuracy  %.3f   precision %.3f   recall %.3f" % (accuracy, precision, recall))
    print("  confusion:            predicted")
    print("                    consistent  tampered")
    print("  actual consistent   %6d    %6d" % (tn, fp))
    print("         tampered     %6d    %6d" % (fn, tp))
    if fn:
        print("  %d tampered profiles were called consistent -- the edits with no internal tell"
              % fn)
    return accuracy


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", help="a dataset written by `sas4_model.py dataset`")
    parser.add_argument("--count", type=int, default=3000, help="rows to generate if no --data")
    parser.add_argument("--depth", type=int, default=4, help="max tree depth")
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()

    rows = load_rows(args.data) if args.data else make_rows(args.count, args.seed)
    train, test = split(rows, 0.3, args.seed)
    positives = sum(r["label"] for r in rows)
    print("data: %d rows (%d tampered, %d consistent), %d train / %d test"
          % (len(rows), positives, len(rows) - positives, len(train), len(test)))

    # Baseline: the rule detector itself, as a feature. Anything a model does has to beat it.
    evaluate("baseline: flag when num_violations > 0",
             lambda r: 1 if r["num_violations"] > 0 else 0, test)

    tree = build_tree(train, 0, args.depth)
    evaluate("decision tree (from scratch, depth %d)" % args.depth,
             lambda r: classify(tree, r), test)
    print("\nlearned tree:")
    print_tree(tree)

    try:
        from sklearn.tree import DecisionTreeClassifier
        X = [[r[f] for f in FEATURES] for r in train]
        y = [r["label"] for r in train]
        clf = DecisionTreeClassifier(max_depth=args.depth, random_state=args.seed).fit(X, y)
        evaluate("scikit-learn DecisionTreeClassifier (for comparison)",
                 lambda r: int(clf.predict([[r[f] for f in FEATURES]])[0]), test)
    except ImportError:
        print("\n(scikit-learn not installed; the from-scratch tree above is the whole story.")
        print(" `py -m pip install scikit-learn` to add the comparison.)")

    print("\nThe ceiling is the point: the errors are tampered saves with no internal tell.")
    print("Catching those needs data a single file does not carry -- how fast the level came,")
    print("how the account sits against millions of others -- which is the server's to see.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
