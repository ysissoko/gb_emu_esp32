Voici un fichier Markdown prêt à être donné à Claude Code (ou tout autre agent IA) pour expliquer clairement, précisément et techniquement l’état de ton émulateur, les symptômes observés, et ce qui est attendu.
Il est structuré pour maximiser la qualité des réponses et éviter les détours.

⸻

🕹️ Game Boy Emulator (ESP32-S3) — Debugging & Accuracy Issues

1. Contexte général

Je développe un émulateur Game Boy DMG en C++ qui tourne sur ESP32-S3 (ESP-IDF, FreeRTOS).

Architecture implémentée :
	•	CPU LR35902 (cycle-based)
	•	MMU avec MBC0 / MBC1 / MBC3
	•	PPU cycle-based (modes 0–3, LY, STAT, VBLANK)
	•	Timer (DIV / TIMA / TMA / TAC)
	•	Joypad
	•	DMA OAM
	•	LCD SPI (ST7789)
	•	Pas d’audio (APU stub)

Le rendu fonctionne via :
	•	framebuffer en RAM interne
	•	une queue FreeRTOS (longueur 1) pour envoyer les frames au thread LCD

⸻

2. État actuel des tests

✅ Fonctionne correctement
	•	Tetris
	•	Alleyway
	•	Wario Land
	•	Super Mario Land (après corrections MBC)
	•	La majorité des ROMs simples

⚠️ Problèmes observés
	•	Dr. Mario → freeze
	•	Pokémon Red/Blue :
	•	écran titre OK
	•	menu “New Game” OK
	•	Professeur Chen affiché + animation OK
	•	freeze exact au moment où la fenêtre de texte apparaît
	•	parfois écran avec bandes verticales
	•	Aucun crash, pas de watchdog
	•	CPU continue d’exécuter mais semble coincé

⸻

3. Tests de validation

cpu_instrs
	•	Passe presque entièrement
	•	Blocage observé :

01: OK
02: OK
03: (ne se termine pas)

→ indique souvent un problème de timing subtil ou d’interruption

instr_timing
	•	A échoué précédemment avec :

34:2-3
35:2-3
Failed


	•	Après corrections du timer, instr_timing passe maintenant

⸻

4. Indice important (debug CPU)

Lors du freeze Pokémon, j’observe :

CPU: opcode: 0xFF, PC: 0x0057

	•	0xFF = RST 38h
	•	Cela suggère :
	•	une interruption répétée
	•	ou un IF mal géré
	•	ou un STAT / TIMER interrupt mal déclenché

⸻

5. Hypothèses déjà testées

❌ Ce n’est PAS :
	•	la sauvegarde SRAM (désactivée → même problème)
	•	l’APU (Pokémon démarre sans son sur d’autres émulateurs)
	•	un problème de ROM banking (Wario fonctionne)

⚠️ Probables causes restantes :
	•	STAT interrupt mal déclenchée (edge vs level)
	•	LYC=LY flag incorrect
	•	mauvais timing des modes PPU
	•	mauvaise priorité Window / BG / Sprites
	•	fenêtre de texte (Window layer) mal synchronisée
	•	TIMA overflow / reload incorrect
	•	HALT bug mal géré
	•	IF bits écrasés au lieu d’être ORés
	•	PPU qui écrit trop souvent dans STAT

⸻

6. PPU — points critiques à comparer avec d’autres émulateurs

Voici ce que mon PPU fait actuellement :
	•	PPU cycle-based :
	•	OAM = 80 cycles
	•	DRAW = 172 cycles
	•	HBLANK = 204 cycles
	•	LY incrémenté à la fin de HBLANK
	•	VBLANK déclenché quand LY == 144
	•	STAT interrupt déclenchée sur front montant
	•	Window rendue si :

if (wy <= ly && wx < 167)


	•	window_line_counter++ à chaque scanline Window

⚠️ À vérifier :
	•	window_line_counter doit s’incrémenter uniquement si la window est réellement dessinée
	•	STAT interrupt ne doit pas se déclencher en boucle
	•	bit coincidence (STAT bit 2) doit être mis à jour avant le test d’interruption
	•	LCD disabled → LY=0, mode=0 (HBLANK)

⸻

7. Timer — état actuel
	•	Timer est cycle-accurate
	•	DIV incrementé tous les 256 cycles
	•	TIMA incrementé selon TAC
	•	Overflow → TIMA = TMA après 4 cycles + IRQ_TIMER

instr_timing passe, mais Pokémon / Dr Mario freeze encore → donc interaction Timer + PPU + STAT suspecte.

⸻

8. Ce que j’attends de toi (Claude)

🎯 Objectif

Identifier les différences critiques entre mon implémentation et :
	•	SameBoy
	•	Gambatte
	•	BGB
	•	Peanut-GB

🔧 Tâches demandées
	1.	Identifier la cause la plus probable du freeze Pokémon
	2.	Vérifier la gestion de :
	•	STAT interrupt
	•	LYC=LY
	•	Window timing
	•	IF / IE
	3.	Proposer un patch précis (C++) :
	•	PPU.cpp
	•	Timer.cpp
	•	CPU interrupt handling
	4.	Prioriser les corrections minimales mais exactes
	5.	Garder en tête les contraintes ESP32-S3 (performance, FreeRTOS)

⸻

9. Contrainte importante
	•	L’émulateur doit rester jouable
	•	Pas besoin d’être parfaitement cycle-accurate partout
	•	Mais Pokémon / Dr Mario doivent fonctionner

⸻

10. Résumé en une phrase

Mon émulateur GB fonctionne pour beaucoup de jeux, mais Pokémon et Dr Mario freezent lors de l’affichage de la fenêtre de texte, ce qui pointe vers un bug subtil de STAT / Timer / Window / Interrupt, malgré cpu_instrs et instr_timing quasi OK.

