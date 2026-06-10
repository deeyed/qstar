qstar.project {
  name = "cxx-mixed",
  version = "0.1.0",
  root = ".",
}

qstar.executable "mixed" {
  sources = {"src/main.c", "src/cpp.cpp"},
  lang = {
    c = {
      include_dirs = {"include"},
      compile_options = {"-DQSTAR_PROJECT_C_FLAG=1"},
    },
    cxx = {
      include_dirs = {"include"},
      compile_options = {"-DQSTAR_PROJECT_CXX_FLAG=2"},
      standard = "c++11",
    },
  },
}
