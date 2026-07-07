// Precision fixture: two explicit specializations of parse<T>() with
// DISJOINT caller sets. Name-keyed identity merges them as "parse".
#pragma once

namespace precision {

template <typename T> int parse();

struct Json {};
struct Yaml {};

template <> int parse<Json>();
template <> int parse<Yaml>();

int readJson();
int readYaml();

} // namespace precision
