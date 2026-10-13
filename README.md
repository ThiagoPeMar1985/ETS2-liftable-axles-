# ts-liftable-axles

Plugin experimental para **Euro Truck Simulator 2 / ATS 1.61** (singleplayer) que
permite escolher **quais eixos da carreta levantam** com a tecla nativa de
levantar eixos — e que levanta automaticamente o **primeiro eixo ao engatar a
ré**, como nas carretas brasileiras de 4º eixo autodirecional (Van der Leest).

*Experimental ETS2/ATS 1.61 plugin (singleplayer): pick which trailer axles
respond to the game's own axle-lift key, plus automatic first-axle lift when
reverse gear engages — like Brazilian self-steering fourth-axle trailers.*

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
- **Tecla nativa**: nenhum keybind extra — toda a animação e física continuam
  sendo feitas pelo próprio jogo. A seleção do usuário também funciona com a
  carreta vazia ou carregada.
- **Persistência**: a seleção de eixos e a opção do modo ré são salvas em
  `%APPDATA%\ts-liftable-axles\settings.ini` por posição da carreta na cadeia.

### Regras de segurança

- Somente eixos configurados como levantáveis pelo jogo/mod podem ser
  selecionados.
- Com os eixos no alto ou em movimento, os checkboxes ficam bloqueados e o
  plugin não altera nada — mude a seleção com os eixos no chão.
- O modo ré pode ser desligado no checkbox "Lift first axle in reverse".

### Instalação

Copie `ts-liftable-axles.dll` para:

```
<Euro Truck Simulator 2>\bin\win_x64\plugins\
```

## English

### What it does

- **Per-axle selection**: in the plugin window (`Delete` → Trailer
  Manipulation), each trailer lists its natively liftable axles with a
  checkbox. Checked axles are the ones the game's own "lift trailer axles" key
  raises. Non-liftable axles are shown but cannot be selected.
- **Reverse-gear assist**: with every axle on the ground, engaging reverse
  keeps only the **first liftable axle** selected and raises it automatically.
  Leaving reverse lowers it and restores your saved selection. If axles are
  already up when reverse engages, nothing changes.
- **Native key**: no extra keybind — the game itself runs the whole lift
  animation and physics. The selection works with the trailer loaded or empty.
- **Persistence**: per-trailer-position selection and the reverse-assist option
  are stored in `%APPDATA%\ts-liftable-axles\settings.ini`.

### Install

Copy `ts-liftable-axles.dll` to `<ETS2>\bin\win_x64\plugins\`.

## Build / Compilar

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target ts-liftable-axles
```

The DLL lands in `build/Release/ts-liftable-axles.dll`.

## Credits / Créditos

Forked from [ts-extra-utilities](https://github.com/DarioVahva/ts-extra-utilities)
by Dario Wouters (trailer steering and keybind framework kept), MIT license.
