qstar.project {
  name = "bad-unsupported-source",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
}

qstar.executable "bad_objc" {
  sources = {
    "src/AppDelegate.m",
  },
}
