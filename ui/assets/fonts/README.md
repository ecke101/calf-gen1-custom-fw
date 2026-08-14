# Embedded UI fonts

The replacement UI embeds these unmodified Noto font binaries so the camera
does not depend on a font file being present in its root filesystem:

- `NotoSans-Regular.ttf` for Latin, Greek, Cyrillic, and general text;
- `NotoSansSymbols-Regular.ttf` for arrows and common technical symbols; and
- `NotoSansSymbols2-Regular.ttf` for additional interface, media, and pictorial
  symbols.

They were copied from `notofonts/noto-fonts` main commit
`ffebf8c1ee449e544955a7e813c54f9b73848eac`. All three are distributed under
the SIL Open Font License 1.1 in `OFL.txt`.

The checked-in SHA-256 digests are:

```text
b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5  NotoSans-Regular.ttf
8f02f31959bbdf6061547a188248e13f84dc5fdd940326ec494675f453f072bb  NotoSansSymbols-Regular.ttf
630846d528dbe4c4981370a4d0a9475a1fd1491a129bb411f8e157cdb5de13c6  NotoSansSymbols2-Regular.ttf
```

The fonts intentionally do not attempt to cover CJK or color emoji. Add a
fallback font and a corresponding generated data symbol when one of those
scripts becomes a UI requirement.
