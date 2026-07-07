#include "templates.hpp"

namespace precision {

template <> int parse<Json>() { return 1; }
template <> int parse<Yaml>() { return 2; }

int readJson() { return parse<Json>(); }
int readYaml() { return parse<Yaml>(); }

} // namespace precision
