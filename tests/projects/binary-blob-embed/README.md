# Binary Blob Embed Corpus

Round 57 corpus for the QStar binary blob flow.

This project models a binary-like fixture embed without hardcoding a domain
specific packager into QStar:

- `fixtures/payload.elf` is a binary-like input.
- `qstar.custom_target "embed_object"` turns it into `generated/payload.o`.
- `qstar.executable "probe"` consumes the generated object as a link input.

`format = "object"` classifies the output as an object artifact. QStar does not
compile that source again; it tracks the generated file as a final link input.
