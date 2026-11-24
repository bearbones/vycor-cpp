struct Options {
  int port = 80;
  int timeout = 30;
  bool verbose = false;
};

class OptionsBuilder {
  Options options;
public:
  OptionsBuilder& port(int value) { options.port = value; return *this; }
  OptionsBuilder& timeout(int value) { options.timeout = value; return *this; }
  OptionsBuilder& verbose(bool value) { options.verbose = value; return *this; }
  Options build() const { return options; }
};

Options server(int base) {
  Options cfg{};
  cfg.verbose = true;
  cfg.port = base + 1;
  return cfg;
}

Options worker() {
  Options cfg;
  cfg.timeout = 10;
  return cfg;
}

int main() {
  auto s = server(8000);
  auto w = worker();
  return !(s.verbose && s.port == 8001 && s.timeout == 30 &&
           !w.verbose && w.port == 80 && w.timeout == 10);
}
