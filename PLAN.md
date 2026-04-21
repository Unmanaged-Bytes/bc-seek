# bc-seek — Plan d'implémentation

**Statut** : v0 (2026-04-21). En cours de cadrage, rien n'est implémenté.
**Objectif** : CLI de recherche de fichiers (`find`-like) plafonnant le hardware via walk parallèle 3-phases sur la stack bc-* moderne. Cible primaire : battre `find(1)` et rivaliser avec `fd` (Rust) sur corpus multi-100k fichiers.

---

## 1. Contexte et source

- **Source legacy** : `/home/younes-benmoussa/Documents/old-dev/archive/bc-seek-20260410/` (5 sources, 1 test cmocka).
- **Cible** : `~/Workspace/tools/bc-seek/` — tool consumer de la stack bc-* livrée.
- **Queue projet** : documenté comme prochain tool après bc-hash/bc-duplicate (cf. auto-memory `project_next_projects_queue.md`).
- **Pattern de référence** : `~/Workspace/.claude/docs/parallel-walk-pattern.md` — walk parallèle mesuré sur bc-hash iter8.

Le code legacy consomme l'ancienne stack (`bc-commun`, `bc-memory`, `bc-filesystem`, `bc-application`, `bc-terminal`). Il faut **tout réécrire** sur la stack actuelle (`bc-core`, `bc-allocators`, `bc-containers`, `bc-concurrency`, `bc-io`, `bc-runtime`). La spec CLI et la taxonomie des filtres restent utiles comme référence fonctionnelle.

---

## 2. Objectifs

### Objectifs fonctionnels (v1)

1. `bc-seek [OPTIONS] [PATH...]` — walk + affichage des chemins matchant.
2. Prédicats par fichier :
   - `--name GLOB` (fnmatch sur basename, case-sensitive et `--iname` insensitive)
   - `--path GLOB` (fnmatch sur chemin complet)
   - `--type f|d|l` (fichier régulier, dir, symlink)
   - `--size +N|-N|N[cwbkMG]` (syntaxe find)
   - `--mtime +N|-N|N` / `--newer PATH`
   - `--perm MODE` (octal strict)
   - `--min-depth N` / `--max-depth N`
3. Mode walk :
   - `--hidden` (inclure dotfiles, off par défaut comme `fd`)
   - `--no-ignore` (ignorer `.gitignore`/conventions, off → respecter)
   - `--follow-symlinks`
   - `--one-file-system`
4. Output :
   - Défaut : une ligne par match sur stdout
   - `--null` / `-0` : null-terminated (xargs -0 compatible)
   - `--output PATH` / `--output -` : redirection
5. Parallélisme :
   - `--threads auto|0|N` (auto = physical cores via bc-concurrency, 0 = single-thread, N = explicit)

### Objectifs non-fonctionnels

- Zéro dépendance externe (seulement libc + bc-*).
- C11 strict, `werror`, cppcheck clean, ASAN/TSAN/UBSAN clean.
- Build release : LTO + `-march=native` + PGO optionnel.
- Tests cmocka verts en `bc-test bc-seek` + bench reproductible.
- Mémoire : zéro leak (ASAN), zéro data race (TSAN), alloc per-worker stricte.

### Non-goals (explicites, à garder hors scope v1)

- `-exec` et `-print0 | xargs` integration (use `-0` + xargs dehors).
- Regex (PCRE/POSIX). Fnmatch/glob only v1.
- Base de données indexée (c'est le domaine de `plocate`/`mlocate`).
- Content grep (domaine ripgrep/`bc-grep` futur).
- Expression booléenne complexe `-and -or -not` à la find. V1 = conjonction AND seulement.
- Support Windows / macOS. Linux only (utilise getdents64, O_NOFOLLOW Linux-specific).

---

## 3. Compétiteurs à mesurer

| Outil | Langage | Modèle | Commentaire |
|---|---|---|---|
| `find` (GNU findutils 4.10) | C | Walk séquentiel | Référence POSIX. Très riche en prédicats. Pas de parallélisme intrinsèque. |
| `fd` / `fdfind` (10.2 Debian) | Rust | Walk parallèle via `ignore` crate | Cible directe. Utilise jwalk pour walk parallèle, respecte `.gitignore`. Réputation de vitesse. |
| `find ... \| xargs -P16` | C + shell | Pipeline parallèle | Baseline "parallélisme pauvre". |
| `plocate` | C++ | DB indexée | Hors scope (model différent) mais à mentionner en contexte. |

Corpus cible : `/var/benchmarks` (confirmé présent sur ws-desktop-00, ~954k fichiers, 16 GB d'après docs bc-hash iter8).

Scenarios de bench (par ordre de criticité) :
1. **Walk seul** (`--type f`, pas d'autre filtre) — teste la vitesse brute du walk.
2. **Name match** (`--name "*.txt"`) — teste le fast-path fnmatch avec short-circuit.
3. **Type+size** (`--type f --size +1M`) — force le stat lazy.
4. **Deep hierarchy** (sous-dir avec profondeur 20+) — teste la queue MPMC.
5. **Peu de matches** (`--name "*.unique"`) — teste le pipeline quand output est minimal.
6. **Beaucoup de matches** (pas de filtre → dump full tree) — teste le bottleneck stdout.

Mesure : 10 runs warm (médiane + stddev), 3 runs cold (drop_caches, sudo requis → demander avant).

---

## 4. Architecture

### 4.1 Layout fichiers (cible)

```
tools/bc-seek/
├── meson.build              # projet + deps bc-*
├── meson_options.txt
├── LICENSE (MIT)
├── README.md
├── CHANGELOG.md
├── cppcheck-suppressions.txt
├── valgrind.supp
├── include/internal/        # headers privés
│   ├── bc_seek_types.h
│   ├── bc_seek_cli.h
│   ├── bc_seek_discovery.h
│   ├── bc_seek_filter.h
│   ├── bc_seek_output.h
│   └── bc_seek_error_collector.h
├── src/
│   ├── bc_seek_main.c       # entry point, bc_runtime_create/run/destroy
│   ├── cli/
│   │   └── bc_seek_cli_spec.c
│   ├── discovery/
│   │   ├── bc_seek_discovery_walk_sequential.c   # iter1 baseline
│   │   └── bc_seek_discovery_walk_parallel.c     # iter2 3-phases
│   ├── filter/
│   │   ├── bc_seek_filter_compile.c              # compile predicates
│   │   └── bc_seek_filter_evaluate.c             # per-entry eval
│   ├── output/
│   │   └── bc_seek_output_simple.c               # line-by-line + null
│   ├── error/
│   │   └── bc_seek_error_collector.c
│   └── bench/
│       ├── bc_seek_throughput.c                  # opt. characterization
│       └── bc_seek_dispatch_decision.c           # mono vs multi
├── tests/
│   └── (13+ fichiers cmocka, miroir bc-hash)
├── scripts/
│   └── bench.sh             # cloné depuis bc-hash/bc-duplicate
└── debian/
    └── (control, changelog, copyright, rules)
```

### 4.2 Pipeline d'exécution

```
argv ──▶ bc_runtime_cli_parse ──▶ bc_seek_cli_spec
                                        │
                                        ▼
                                 compile predicates (immutable struct)
                                        │
                          ┌─────────────┴─────────────┐
                          ▼ (si < seuil break-even)   ▼ (si ≥ seuil ou --threads)
                    walk séquentiel              walk parallèle 3-phases
                          │                           │
                          ▼                           ▼
                    filter per-entry (inline)   Phase A: walk + filter per-worker
                          │                     Phase B: merge (optional: sort)
                          ▼                     Phase C: pas nécessaire ici
                     output line                       │
                                                       ▼
                                               output line (sequential)
```

**Décision clé** : dans bc-seek, la Phase C du pattern bc-hash n'existe pas — le filtering peut se faire **inline dans Phase A** (per-worker pendant le walk), parce que chaque prédicat est évalué sur une entrée isolée. Phase B se contente de merger les matches vers stdout en ordre non-déterministe (ou trié si `--sort`).

Ça simplifie vs bc-hash (2 phases au lieu de 3). Mais le skeleton de bc-hash `discovery/walk_parallel.c` reste la référence pour Phase A.

### 4.3 Filter évaluation — short-circuit ordering

L'ordre d'évaluation des prédicats impacte la perf si on le choisit bien :

1. **`--max-depth` / `--min-depth`** — test sur depth (déjà connu), O(1), pas de syscall.
2. **`--type`** — test sur `d_type` de readdir, O(1). **Si `d_type == DT_UNKNOWN`, différer** à après stat.
3. **`--name` / `--iname`** — fnmatch sur basename. O(name_length). Pas de syscall.
4. **`--path`** — fnmatch sur path complet. Idem.
5. **`--size`, `--mtime`, `--newer`, `--perm`** — nécessitent `stat`. **Ne stat que si un de ces prédicats est actif et tous les prédicats précédents ont matché.**

Ce stat-lazy est la clé pour ne pas régresser sur le walk pur : sans aucun prédicat stat-required, bc-seek n'appelle **jamais** stat, juste getdents64.

### 4.4 Prédicat compilé (struct immuable)

```c
typedef struct {
    bool has_name_filter;
    const char* name_glob;
    bool name_case_insensitive;

    bool has_path_filter;
    const char* path_glob;

    bool has_type_filter;
    bc_seek_entry_type_t type;

    bool has_size_filter;
    int64_t size_threshold;
    bc_seek_size_op_t size_op;    // GT, LT, EQ

    bool has_mtime_filter;
    int64_t mtime_threshold_seconds;
    bc_seek_time_op_t mtime_op;

    size_t min_depth;
    size_t max_depth;   // 0 = unlimited

    bool require_stat;  // = has_size || has_mtime || has_perm || has_newer
} bc_seek_predicate_t;
```

`bc_seek_predicate_t` est **read-only partagé** entre tous les workers — conforme au pattern (config partagée read-only).

### 4.5 Output : sérialiser sans bottleneck

Trap classique : un walk parallèle 6-core qui `printf` dans `stdout` sous lock devient séquentiel au write. Stratégie :

- Par worker : écrire dans un buffer local (`bc_containers_vector<char>` ou FILE* in-memory). Append matches + `\n`.
- Phase merge : écrire les buffers vers stdout en bloc (`write(1, buf, len)` — une seule syscall par worker).
- `--null` : remplacer `\n` par `\0` dans le builder.

Avec `stdout` non-TTY (pipe), `setvbuf(stdout, NULL, _IOFBF, 1<<16)` dans main pour amplifier le buffering.

---

## 5. CLI surface détaillée (spec v1)

```
bc-seek [GLOBAL OPTIONS] [COMMAND] [OPTIONS] [PATH...]

Commands:
  find                   Recherche (default si omis)

Global options:
  --threads auto|0|N     Worker count (default: auto)
  --help, -h
  --version, -V

find options:
  --name GLOB            Basename fnmatch (case-sensitive)
  --iname GLOB           Basename fnmatch (case-insensitive)
  --path GLOB            Full-path fnmatch
  --type TYPE            f | d | l  (file | directory | symlink)
  --size SIZE            +N | -N | N with suffix c|w|b|k|M|G (find syntax)
  --mtime DAYS           +N | -N | N (days ago)
  --newer PATH           Modified more recently than PATH
  --perm OCTAL           Exact permission bits (e.g. 755)
  --min-depth N          (default 0, root depth = 0)
  --max-depth N          (default unlimited)
  --hidden               Include dotfiles (default: excluded)
  --no-ignore            Don't respect .gitignore or ignored dirs like .git (default: respect)
  --follow-symlinks      (default: off, O_NOFOLLOW)
  --one-file-system      Don't cross filesystem boundaries (st_dev)
  --output PATH | -      Output destination (default: stdout)
  --null, -0             Null-terminated output (xargs -0)

Positional:
  [PATH...]              Roots to search (default: ".")
```

Note vs legacy : le legacy prend un seul `PATTERN` positionnel, on élargit vers N paths comme `find`/`fd`. Le "pattern" legacy devient `--name GLOB`.

---

## 6. Itérations

Les itérations sont **commit-sized** : une itération = un thème, des tests RED-GREEN, une mesure, un commit (ou plusieurs commits atomiques).

### iter1 : squelette + walk séquentiel + port CLI
**Scope** :
- Meson projet, deps bc-* wired.
- Headers privés + types (`bc_seek_types.h`, predicate struct).
- CLI spec via `bc_runtime_cli_spec_t` (port depuis legacy, moderniser).
- Walk séquentiel pur : openat/fdopendir/readdir, récursion, stat lazy.
- Filter minimal : `--name`, `--type`, `--max-depth`.
- Output simple stdout line-by-line.
- Tests cmocka : CLI parse, filter basics, fixture tmpfs.
- Script `bench.sh` v0 : bc-seek vs find, corpus petit (~/Downloads).

**Sortie** : bc-seek fonctionnel mono-thread. Baseline mesurée vs find et fd.

### iter2 : walk parallèle 3-phases
**Scope** :
- Copier-adapter `bc_hash_discovery_walk_parallel.c` pour bc-seek.
- MPMC queue dirs, per-worker slot avec vector de matches.
- Atomic termination counter (BEFORE push / AFTER process).
- Merge post-barrière : foreach_slot → write vers stdout (bloc).
- Tests : fixture multi-dir, comparaison output mono vs multi (mêmes matches, ordre non-significant).
- Bench : corpus `/var/benchmarks`, scaling 1/2/4/8/16 threads.

**Sortie** : battement `find` sur corpus. Mesure scaling cores.

### iter3 : prédicats étendus + stat-lazy rigoureux
**Scope** :
- `--size`, `--mtime`, `--newer`, `--perm`, `--iname`, `--path`.
- Logic stat-lazy : ne stat que si `predicate.require_stat` et prédicats non-stat ont matché.
- Tests : chaque prédicat isolé (RED-GREEN), combos, edge cases (size 0, mtime future, perm 0).
- Bench : scenario "type+size" ajouté.

### iter4 : dispatch decision + throughput characterization
**Scope** :
- `bc_seek_throughput.c` : mesurer `parallel_startup_cold` + `per_file_cost_warm` sur le host. Cache dans `$XDG_CACHE_HOME/bc-seek/throughput.txt`.
- `bc_seek_dispatch_decision.c` : formule mono vs multi (copier `bc_hash_dispatch_decision.c`).
- `--threads auto` résout sur mono ou multi selon la formule.
- Tests : forcer mono/multi, vérifier cohérence.

### iter5 : ignore conventions + options restantes
**Scope** :
- `--hidden`, `--no-ignore`, `--one-file-system`, `--follow-symlinks` complets.
- Liste par défaut de dirs ignorés : `.git`, `.hg`, `.svn`, `node_modules`, `target`, `__pycache__`, etc. (doc explicite).
- `--null` / `-0`.
- Tests : walk avec .git présent, one-fs sur un mount point `/tmp` (tmpfs).

### iter6 (optionnel) : perf polish
**Scope** :
- Valider que le workflow PGO de bc-hash (`scripts/build-pgo.sh`) s'applique à bc-seek.
- Investigation perf : perf record / perf report, identifier hot paths.
- Potentiel : output writer dédié si stdout devient bottleneck.
- Comparer `fd` → si on est derrière, profiler pour comprendre pourquoi.

---

## 7. Perf targets

Sur ws-desktop-00 (AMD 5700G, 8 cores physiques, 32 GB RAM, DDR4-3200), corpus `/var/benchmarks` (~954k files) :

| Scenario | find (baseline) | cible bc-seek | fd (référence) |
|---|---|---|---|
| Walk seul, type=f | à mesurer | ≤ 40% du temps find | à mesurer |
| Name match `*.txt` | à mesurer | ≤ 40% du temps find | parité ou mieux |
| Type + size | à mesurer | ≤ 50% du temps find | parité ou mieux |
| Cold cache (drop_caches) | à mesurer | ≤ 60% du temps find | parité |

Scaling cœurs : ≥ 5.5x effectifs sur 8 cores physiques (repère bc-hash iter8 : 6.4x sur hash). Seek devrait faire pareil ou mieux (I/O bornée dirent, peu de compute).

---

## 8. Risques et points ouverts

### Risques identifiés

- **fd est déjà rapide** : il utilise jwalk (Rust, work-stealing pool) + ignore crate (bien optimisé). Parité demande une impl propre. Si on reste derrière, investiguer pourquoi (readdir buffer size, ordre pred, bottleneck output).
- **Ordre non-déterministe** : walk parallèle ne garantit pas l'ordre. Scripts qui dépendent de l'ordre find → casser. Option `--sort` à ajouter si besoin réel (iter6).
- **Corpus `/var/benchmarks`** : cold cache demande sudo pour `echo 3 > /proc/sys/vm/drop_caches`. **À demander à l'utilisateur avant chaque run cold.**
- **cppcheck + `werror`** : la stack legacy avait `-Wconversion -Wshadow`, prévoir fixes sur signedness (size_t vs ssize_t sur readdir).
- **O_NOFOLLOW strict** : avec `--follow-symlinks`, il faut un inode set pour éviter cycles. bc-io a `bc_io_file_inode_set_*`, mais il faut le wrapper per-worker-merge proprement (le pattern doc mentionne "per-worker inode set + merge à la fin").

### Points à valider avant iter2

- Benchmark baseline iter1 sur corpus réel : est-ce que le mono nous met déjà à 60% de find ? Si oui, le gain parallèle est moindre qu'anticipé.
- fd support-il `.gitignore` par défaut sur ce corpus ? Ça peut sur/sous-compter matches. Comparer avec `fd -uu` (unrestricted).

### Points ouverts (à décider plus tard)

- Support regex (PCRE) en v2 ? Ajout `--regex` vs `--name`.
- `--exec` ? Probablement non — `bc-seek ... -0 | xargs -0 -P CMD` couvre le cas.
- Sortie "long format" type `ls -la` avec `bc_runtime_table` (quand ce header existera) ? À discuter.

---

## 9. Discipline et autonomie

### Red-Green obligatoire (cf. CLAUDE.md global)

Chaque bug fix pendant le développement :
1. Test qui reproduit le bug → échoue (RED).
2. Fix appliqué.
3. Test passe (GREEN).
4. Commit test + fix ensemble ou séquentiellement (test d'abord).

### Checkpoints commits

- Un commit = une feature ou un fix testé. Pas de WIP à la main.
- Chaque itération produit au minimum 1 commit, idéalement 3-5 atomiques.
- Messages commit en français, concis, suivant conventions du repo (voir `git log` une fois init).

### Autonomie — oui, avec ces garde-fous

**Je peux travailler en autonome sur** :
- Implémentation, tests, benchmarks itération par itération.
- Runs `bc-build`, `bc-test`, `bc-sanitize asan/tsan/ubsan`, `bc-check`, `bc-bench`.
- Rédaction de code, refactoring local, commits locaux.
- Comparaison perf avec find/fd sur corpus en mode warm.
- Itération sur perf après mesure.

**Je demande confirmation avant** :
- `bc-install bc-seek` (modifie `/usr/local`, partagé).
- Runs cold-cache (sudo drop_caches).
- Push git (remote).
- Modification de libs bc-* amont (bc-io, bc-concurrency) — si une primitive manque, je propose un patch et demande.
- Création de fixtures disque larges (>1 GB) ou écriture dans `/var/*`.

**Je ne fais jamais** :
- `git push --force`, `git reset --hard`, `git restore` sur non-commité.
- Désactiver hooks, sanitizers, cppcheck.
- Ajouter une dépendance externe sans validation explicite.

### Livrable attendu par itération

- Code + tests verts (`bc-test bc-seek release` **et** `bc-test bc-seek debug`).
- Sanitizers au vert sur un run minimum (ASAN en itération normale, TSAN quand le parallèle bouge).
- Mesure reproductible loguée dans `build/perf-logs/iterN.md` (comme bc-hash).
- Commit(s) atomiques avec messages concis.
- Mise à jour de ce PLAN.md : statut de l'itération, mesures clés, leçons apprises.

---

## 10. Prochaine étape

**iter1 — squelette** :
1. Créer `meson.build` + `meson_options.txt` (template bc-hash).
2. Créer `src/bc_seek_main.c` (minimal runtime).
3. Créer `src/cli/bc_seek_cli_spec.c` (portage legacy → bc_runtime_cli).
4. Créer `src/discovery/bc_seek_discovery_walk_sequential.c` (walk mono, types + name + max-depth).
5. Créer `src/output/bc_seek_output_simple.c` (stdout line).
6. 3 tests cmocka : CLI parse, filter name, walk sequential sur fixture.
7. `bc-build bc-seek debug` + `bc-test bc-seek debug` verts.
8. Commit atomique.
9. Premier bench naïf vs find sur `~/Workspace` (corpus sûr, petit).

À lancer seulement après feu vert utilisateur sur ce plan.
