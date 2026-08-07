# Contributing to ffapv

This repository accepts pull requests **only for the codec integrations it
maintains**:

- **APV** — the OpenAPV encoder wrapper (`libavcodec/liboapvenc.c`) and the
  related APV pieces (native decoder, parser, muxing/demuxing)
- **EVC** — the xeve/xevd wrappers (`libavcodec/libxeve.c`,
  `libavcodec/libxevd.c`) and the related EVC pieces

Changes to any other part of the FFmpeg code base are out of scope here.
Please submit those to the FFmpeg project itself — see
[https://ffmpeg.org/developer.html#Contributing](https://ffmpeg.org/developer.html#Contributing)
(patches go to [Forgejo](https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls) or the
[ffmpeg-devel mailing list](https://ffmpeg.org/mailman/listinfo/ffmpeg-devel)).
This tree regularly merges upstream FFmpeg, so fixes accepted there arrive
here as well.

## How to contribute

1. Fork the repository and create a feature branch from `main`.
2. Build and test your change (see the Building section of the README).
3. Keep commit subjects to a single line and sign off your commits
   (`git commit -s`).
4. Open a pull request against `main` describing what the change does and how
   it was tested.

When opening a PR from a fork, please **enable "Allow edits from
maintainers"** — it lets maintainers rebase your branch or apply small review
fixes directly, which shortens the review cycle considerably.
