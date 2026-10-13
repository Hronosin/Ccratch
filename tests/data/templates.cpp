// C++ is not modelled yet: templates, classes and namespaces show up as
// "Other". This file only checks that C++ parses at all.
namespace shrine {

template <typename T>
T bigger(T a, T b) { return a < b ? b : a; }

}

int main() { return shrine::bigger(1, 2) - 2; }
