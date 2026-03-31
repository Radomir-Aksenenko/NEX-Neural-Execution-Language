#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>

struct Node {
    int val;
    Node* left;
    Node* right;
    Node(int v, Node* l = nullptr, Node* r = nullptr)
        : val(v), left(l), right(r) {}
};

Node* insert(Node* t, int v) {
    if (!t)          return new Node(v);
    if (v < t->val)  return new Node(t->val, insert(t->left,  v), t->right);
    if (v > t->val)  return new Node(t->val, t->left, insert(t->right, v));
    return t;
}

bool search(Node* t, int v) {
    if (!t)           return false;
    if (v == t->val)  return true;
    if (v < t->val)   return search(t->left,  v);
    return                   search(t->right, v);
}

void inorder(Node* t, std::vector<int>& out) {
    if (!t) return;
    inorder(t->left, out);
    out.push_back(t->val);
    inorder(t->right, out);
}

Node* build(const std::vector<int>& xs) {
    Node* t = nullptr;
    for (int v : xs) t = insert(t, v);
    return t;
}

double mean(const std::vector<int>& xs) {
    double s = 0;
    for (int x : xs) s += x;
    return s / xs.size();
}

double variance(const std::vector<int>& xs) {
    double m = mean(xs), s = 0;
    for (int x : xs) s += (x - m) * (x - m);
    return s / xs.size();
}

double stddev(const std::vector<int>& xs) { return std::sqrt(variance(xs)); }

double median(std::vector<int> xs) {
    std::sort(xs.begin(), xs.end());
    int n = xs.size();
    return (n % 2 == 0) ? (xs[n/2-1] + xs[n/2]) / 2.0 : xs[n/2];
}

int main() {
    std::vector<int> data = {64, 34, 25, 12, 22, 11, 90, 45, 67, 3};
    Node* tree = build(data);

    std::vector<int> result;
    inorder(tree, result);

    std::cout << "Sorted:    ";
    for (int x : result) std::cout << x << " ";
    std::cout << "\nMean:      " << mean(result)
              << "\nStd dev:   " << stddev(result)
              << "\nMedian:    " << median(result)
              << "\nSearch 45: " << std::boolalpha << search(tree, 45)
              << "\nSearch 99: " << std::boolalpha << search(tree, 99) << "\n";
    return 0;
}
