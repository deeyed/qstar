qstar.exe "mixed" {
  sources = {"src/main.c", "src/cpp.cpp"},
  include_dirs = {"include"},
  cflags = {"-DQSTAR_PROJECT_C_FLAG=1"},
  cxxflags = {"-DQSTAR_PROJECT_CXX_FLAG=2"},
  cxx_standard = "c++11",
}
