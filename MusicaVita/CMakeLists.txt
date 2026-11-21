name: Compilar VPK

on:
  push:
    branches: [ main ]
  workflow_dispatch:

jobs:
  build:
    runs-on: ubuntu-latest
    container: vitasdk/vitasdk:latest

    steps:
      - name: Baixar o codigo
        uses: actions/checkout@v4

      - name: Compilar
        run: |
          cmake -S . -B build
          cmake --build build

      - name: Enviar VPK
        uses: actions/upload-artifact@v4
        with:
          name: MusicaVita-vpk
          path: build/MusicaVita.vpk

      - name: Enviar ELF (para depurar crashes)
        uses: actions/upload-artifact@v4
        with:
          name: MusicaVita-elf
          path: build/MusicaVita.elf
