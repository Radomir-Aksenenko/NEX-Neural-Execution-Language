class Node:
    def __init__(self, val, left=None, right=None):
        self.val   = val
        self.left  = left
        self.right = right

def insert(tree, val):
    if tree is None:
        return Node(val)
    if val < tree.val:
        return Node(tree.val, insert(tree.left, val), tree.right)
    if val > tree.val:
        return Node(tree.val, tree.left, insert(tree.right, val))
    return tree

def search(tree, val):
    if tree is None:       return False
    if val == tree.val:    return True
    if val < tree.val:     return search(tree.left,  val)
    return                        search(tree.right, val)

def inorder(tree):
    if tree is None: return []
    return inorder(tree.left) + [tree.val] + inorder(tree.right)

def build(values):
    tree = None
    for v in values:
        tree = insert(tree, v)
    return tree

def mean(xs):
    return sum(xs) / len(xs)

def variance(xs):
    m = mean(xs)
    return sum((x - m) ** 2 for x in xs) / len(xs)

def std_dev(xs):
    return variance(xs) ** 0.5

def median(xs):
    s = sorted(xs)
    n = len(s)
    return (s[n//2-1] + s[n//2]) / 2 if n % 2 == 0 else s[n//2]

# ── main ──────────────────────────────────────────
data   = [64, 34, 25, 12, 22, 11, 90, 45, 67, 3]
tree   = build(data)
result = inorder(tree)

print(f"Sorted:    {result}")
print(f"Mean:      {mean(result):.2f}")
print(f"Std dev:   {std_dev(result):.2f}")
print(f"Median:    {median(result):.2f}")
print(f"Search 45: {search(tree, 45)}")
print(f"Search 99: {search(tree, 99)}")
