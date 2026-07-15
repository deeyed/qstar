qstar.project {
  name = "typed-dependency-negative-imported",
  version = "0.1.0",
  root = ".",
}

qstar.imported "bad" {
  artifacts = {
    generic = {
      {
        id = "archive",
        role = "link",
        path = "vendor/libvendor-default.a",
        primary = true,
      },
    },
  },
}
