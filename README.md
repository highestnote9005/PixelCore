# PocketGBA

Emulador de Game Boy Advance para Android. Núcleo em C (CPU ARM7TDMI ARM+Thumb, vídeo, som, DMA, timers, saves SRAM/Flash/EEPROM, BIOS emulada) e interface em Flutter (controles de toque multitoque, biblioteca, save states).

Não vem com jogos nem BIOS. Use apenas ROMs de jogos que você possui.

## Gerar o APK

### Opção A: no seu computador

Precisa de Flutter e do Android SDK (o NDK e o CMake o Gradle baixa sozinho).

```bash
dart run tool/setup_android.dart   # gera android/, liga o núcleo em C ao Gradle, copia ícones
flutter pub get
flutter build apk --release
```

O APK fica em `build/app/outputs/flutter-apk/app-release.apk`. Passe para o celular e instale (permita "instalar apps desconhecidos").

### Opção B: pelo GitHub, sem instalar nada

1. Suba esta pasta para um repositório seu no GitHub.
2. Aba **Actions** → **Gerar APK** → **Run workflow**.
3. Quando terminar, baixe o artefato **PocketGBA-apk** (um zip com o `app-release.apk`).

O APK sai assinado com a chave de debug, o que serve para instalar no seu aparelho. Para publicar em loja, configure uma chave própria.

## Como usar

- **Abrir ROM**: escolha um `.gba` (ou `.zip` contendo um). Ela é copiada para a pasta do app e fica na biblioteca.
- **Controles**: D-pad, A, B, L, R, Start e Select na tela. Dá para apertar vários ao mesmo tempo e deslizar de A para B. Teclado e gamepad Bluetooth também funcionam (setas, Z/X, A/S, Enter, Backspace).
- **Menu** (botão no topo ou botão voltar): continuar, reiniciar, 3 slots de save state, velocidade 1x/2x/4x, volume, opacidade dos controles, filtro de imagem, escala inteira, vibração.
- **Saves do jogo** (bateria) são gravados automaticamente a cada 2 s, ao abrir o menu e ao sair do app.

## Estrutura

```
native/   núcleo do emulador em C (gba.c, cpu.inc, mem.inc, ppu.inc, apu.inc) + host.c (thread, áudio OpenSL ES, API para o Dart)
lib/      app Flutter (main, library_page, emulator_page, controls, storage, native)
tool/     setup_android.dart e ícones
```

## Limitações conhecidas

- A BIOS é emulada por software (HLE). Jogos que dependem de detalhes exatos da BIOS original podem se comportar diferente.
- Temporização aproximada: a maioria dos jogos roda bem, mas alguns que exigem precisão de ciclo podem ter falhas.
- Não há cabo link, RTC, sensor solar/giroscópio nem leitor e-Reader.
- Save states só valem para a mesma versão do app.
