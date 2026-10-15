# ts-liftable-axles

Plugin experimental para **Euro Truck Simulator 2 / ATS 1.61** (singleplayer) que
permite escolher **quais eixos da carreta levantam** com a tecla nativa de
levantar eixos, levanta automaticamente o **primeiro eixo ao engatar a ré**
(como nas carretas brasileiras de 4º eixo autodirecional, Van der Leest) e tem
keybinds para **abrir/fechar a carroceria** em mods de reboque que usam o truque
do `g_trailer_cables_mode` (packs AM29 — Librelato sider, Guerra graneleiro...).

*Experimental ETS2/ATS 1.61 plugin (singleplayer): pick which trailer axles
respond to the game's own axle-lift key, automatic first-axle lift on reverse —
and covers open/close keybinds for trailer mods that rig their cover animation
to `g_trailer_cables_mode` (AM29 packs).*

## Português

### O que ele faz

- **Seleção por eixo**: na janela do plugin (tecla `Delete` → Trailer
  Manipulation), cada carreta mostra seus eixos nativamente levantáveis com um
  checkbox. Marque quais devem subir ao apertar a tecla "Levantar eixos da
  carreta" do jogo. Eixos que não são levantáveis aparecem listados, mas sem
  checkbox — forçar um deles quebraria a suspensão.
- **Modo ré (Van der Leest / 4º eixo)**: com todos os eixos no chão, engatar a
  ré mantém somente o **primeiro eixo levantável** selecionado e o ergue
  automaticamente — igual ao sistema real que evita o eixo autodirecional
  travar a carreta na manobra. Ao desengatar a ré, o eixo desce e sua seleção
  anterior volta. Se os eixos já estiverem no alto ao engatar, nada é alterado.
- **Tecla nativa**: nenhum keybind extra para os eixos — toda a animação e
  física continuam sendo feitas pelo próprio jogo. A seleção do usuário também
  funciona com a carreta vazia ou carregada.
- **Abrir/fechar carroceria**: na janela "Trailer System", os botões de tecla
  ao lado de "Abrir carroceria" e "Fechar carroceria" são clicáveis — aperte a
  tecla que quiser bindar. *Abrir* abre todas as carrocerias da composição de
  uma vez (`g_trailer_cables_mode 0`); *Fechar* fecha uma carreta por vez a cada
  apertada (modo 1, 2...). **Funciona apenas em mods de reboque que prendem a
  animação da lona/porta ao modo dos cabos** (packs AM29, por exemplo) — em
  carreta vanilla a tecla só troca o modo dos cabos. Abrir a carroceria solta
  os cabos visuais — o acoplamento vem do próprio mod.
- **Persistência**: a seleção de eixos, o modo ré e os keybinds são salvos em
  `%APPDATA%\ts-liftable-axles\settings.ini`.

### Regras de segurança

- Somente eixos configurados como levantáveis pelo jogo/mod podem ser
  selecionados.
- Com os eixos no alto ou em movimento, os checkboxes ficam bloqueados e o
  plugin não altera nada — mude a seleção com os eixos no chão.
- O modo ré pode ser desligado no switch "Erguer 1o eixo na re".

### Instalação

Copie `ts-liftable-axles.dll` para:

```
<Euro Truck Simulator 2>\bin\win_x64\plugins\
```

## English

### What it does

- **Per-axle selection**: in the plugin window (`Delete` → Trailer System),
  each trailer lists its axles one per row with an on/off switch. Enabled
  axles are the ones the game's own "lift trailer axles" key raises.
  Non-liftable axles are shown as "fixo" and cannot be selected.
- **Reverse-gear assist**: with every axle on the ground, engaging reverse
  keeps only the **first liftable axle** selected and raises it automatically.
  Leaving reverse lowers it and restores your saved selection. If axles are
  already up when reverse engages, nothing changes.
- **Native key**: no extra keybind for the axles — the game itself runs the
  whole lift animation and physics. The selection works with the trailer
  loaded or empty.
- **Covers open/close**: in the "Trailer System" window, click the key buttons
  next to "Abrir carroceria" / "Fechar carroceria" to bind a key. *Open* opens
  every cover in the combination at once (`g_trailer_cables_mode 0`); *Close*
  closes one trailer per press (mode 1, 2...). **Only works on trailer mods
  that rig the cover/door/curtain animation to the cables mode** (e.g. the
  AM29 packs) — on vanilla trailers it just changes the cable mode. Opening
  covers disconnects the visual cables — that coupling comes from the mod.
- **Persistence**: per-trailer-position selection, the reverse-assist option
  and the keybinds are stored in `%APPDATA%\ts-liftable-axles\settings.ini`.

### Install

Copy `ts-liftable-axles.dll` to `<ETS2>\bin\win_x64\plugins\`.

### How to use

1. **Install** the DLL, start the game and attach a trailer.
2. Press **`Delete`** to open the "Trailer System" window (use `Insert` to
   release the mouse cursor so you can click it).
3. **Cover keybinds**: click the key button next to **Abrir carroceria**
   (opens every cover at once) and **Fechar carroceria** (closes one trailer
   per press) and press the key you want. Only effective on mods that rig
   covers to the cables mode (AM29 packs).
4. Expand **Carreta 1** (and any other trailers in the chain). One row per
   axle:
   - **Switch** = that axle is liftable and will rise when you press the
     game's own "lift trailer axles" key.
   - **Dimmed "fixo"** = the axle exists but is not liftable on this trailer,
     so it cannot be selected (this protects the suspension — the game would
     crash trying to raise it).
5. Make your selection **with the axles on the ground**. While any axle is up
   or moving, the switches are locked — lower the axles first.
6. Press the **in-game trailer axle lift key** (the same key as always, no
   extra keybind needed). Only the enabled axles rise, with the normal
   animation and physics.
7. **Reverse-gear assist** (the "Erguer 1o eixo na re" switch at the top of
   the axle section, enabled by default): with every axle on the ground,
   engaging reverse gear automatically selects only the first liftable axle
   and raises it — like the real Van der Leest fourth-axle system. Leaving
   reverse lowers the axle and restores your saved selection.
8. Your axle selection, the reverse-assist option and the keybinds are saved
   automatically (`settings.ini`).

**Troubleshooting**: if a trailer shows "No axle data", it may still be
initializing — give it a second. If it never appears, check `game.log.txt`
for `[liftable_axles]` messages.

## Build / Compilar

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target ts-liftable-axles
```

The DLL lands in `build/Release/ts-liftable-axles.dll`.

## Credits / Créditos

Forked from [ts-extra-utilities](https://github.com/DarioVahva/ts-extra-utilities)
by Dario Wouters (plugin framework kept; trailer steering lives in a separate
plugin), MIT license.
