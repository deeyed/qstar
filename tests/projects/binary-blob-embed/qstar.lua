qstar.project {
  name = "binary-blob-embed",
  version = "0.1.0",
  root = ".",
}

qstar.custom_target "embed_object" {
  inputs = {
    "fixtures/payload.elf",
  },
  outputs = {
    qstar.output("generated/payload.o", {
      format = "object",
    }),
  },
  command = qstar.cli {
    "tools/embed-object.sh",
    qstar.input(0),
    qstar.output(0),
  },
}

qstar.executable "probe" {
  sources = {
    "src/main.c",
    qstar.output("generated/payload.o"),
  },
}
