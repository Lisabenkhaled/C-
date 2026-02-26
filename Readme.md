# Rapport — Portefeuille d’actifs et calcul de rendement

## 1) Présentation du sujet

Le sujet consiste à construire en C++ un mini moteur de **gestion de portefeuille financier** avec les éléments suivants :

- une classe `Asset` représentant un actif financier (nom, prix, rendement attendu, volatilité),
- une classe `Portfolio` capable de gérer plusieurs actifs avec quantités,
- des opérations de gestion (`add/remove`),
- des calculs financiers (rendement attendu, variance/volatilité via matrice de corrélation),
- des surcharges d’opérateurs (`operator+`, `operator[]`),
- une gestion d’erreurs via exceptions (`std::out_of_range`, `std::invalid_argument`),
- un programme principal qui démontre l’utilisation,
- et un usage explicite des conteneurs vus en cours (`vector`, `map`, `set`).

Ce projet va plus loin que le strict minimum : il propose aussi une récupération de données depuis Yahoo Finance (prix + historiques), un calcul automatique de corrélations, des tests unitaires, et une interface web locale en plus de la CLI.


## 2) Mathématiques utilisées pour résoudre le sujet

### 2.1 Pondérations du portefeuille

Pour un actif \(i\), on note :

- \(P_i\) : prix,
- \(q_i\) : quantité,
- \(V_i = P_i q_i\) : valeur de la position,
- \(V = \sum_i V_i\) : valeur totale du portefeuille,
- \(w_i = V_i / V\) : poids de l’actif dans le portefeuille.

Les poids satisfont \(\sum_i w_i = 1\) si \(V>0\).

### 2.2 Rendement attendu du portefeuille

Si \(\mu_i\) est le rendement attendu de l’actif \(i\), alors le rendement attendu du portefeuille est :

\[
\mu_p = \sum_i w_i\mu_i.
\]

C’est exactement ce qui est implémenté dans `Portfolio::expectedReturn()`.

### 2.3 Variance et volatilité avec corrélation

On utilise \(\sigma_i\) (volatilité de l’actif \(i\)) et la corrélation \(\rho_{ij}\) entre actifs \(i\) et \(j\).

La covariance vaut :

\[
\mathrm{Cov}(i,j)=\rho_{ij}\sigma_i\sigma_j.
\]

La variance du portefeuille :

\[
\sigma_p^2 = \sum_i\sum_j w_i w_j \rho_{ij}\sigma_i\sigma_j.
\]

La volatilité est ensuite :

\[
\sigma_p = \sqrt{\sigma_p^2}.
\]

L’implémentation est volontairement « approximative » au sens du sujet (on injecte directement une matrice de corrélation, fournie ou estimée).

### 2.4 Données Yahoo : log-returns et annualisation

Quand les données sont récupérées depuis Yahoo, les rendements journaliers sont calculés en **log-return** :

\[
r_t = \ln\left(\frac{S_t}{S_{t-1}}\right).
\]

Puis, à partir de la moyenne journalière \(\bar r\) et de l’écart-type journalier \(s\), on annualise avec 252 jours de bourse :

\[
\mu_{ann} \approx 252\,\bar r,
\qquad
\sigma_{ann} \approx \sqrt{252}\,s.
\]

C’est un standard raisonnable pour une approximation pédagogique.

---

## 3) Présentation du code (fichier par fichier) + justification d’architecture

> Objectif de cette section : expliquer **chaque fichier**, pourquoi il existe, et pourquoi ce choix de conception est pertinent en C++.

### 3.1 `Asset.hpp` / `Asset.cpp`

**Rôle**
- Encapsuler un actif financier (identité + paramètres de risque/rendement).

**Pourquoi un fichier `.hpp` + `.cpp` ?**
- Séparation interface/implémentation : compilation plus propre, dépendances mieux contrôlées, design orienté objet classique en C++.

**Pourquoi cette classe est nécessaire ?**
- Sans classe `Asset`, on manipulerait des tuples ou structures ad hoc partout, ce qui rendrait le code fragile et dupliqué.
- La validation (nom non vide, prix/volatilité non négatifs) est centralisée.

### 3.2 `Portfolio.hpp` / `Portfolio.cpp`

**Rôle**
- Représenter un portefeuille de positions (`Asset` + quantité).
- Fournir l’API métier : ajout/retrait, accès, fusion, métriques.

**Choix des conteneurs**
- `std::map<std::string, Position>` pour les positions :
  - accès par nom d’actif,
  - unicité de clé,
  - ordre lexical stable (utile pour l’ordre de la matrice de corrélation).
- `std::vector` pour les poids, ordres et calculs matriciels :
  - contigu en mémoire,
  - idéal pour les doubles boucles mathématiques.
- `std::set` via `assetNameSet()` :
  - matérialise explicitement l’unicité,
  - répond à la contrainte « utiliser maps/vectors/sets ».

**Pourquoi `operator+` et `operator[]` ?**
- `operator+` donne une sémantique naturelle de fusion de portefeuilles.
- `operator[]` simplifie l’accès à une position ciblée.

**Pourquoi des validations strictes de matrice ?**
- Taille, diagonale, bornes, symétrie sont vérifiées pour éviter des calculs silencieusement faux.
- En finance, valider les hypothèses d’entrée est plus important que « laisser passer ».

### 3.3 `Yahoo.hpp` / `Yahoo.cpp`

**Rôle**
- Connecter le moteur de portefeuille à des données de marché.
- Convertir des séries de prix en statistiques exploitables (\(\mu\), \(\sigma\), corrélations).

**Pourquoi ce module séparé ?**
- Séparation de responsabilités :
  - `Portfolio` = logique métier,
  - `Yahoo` = acquisition de données et pré-traitement.
- Facilite les tests : on peut tester `Portfolio` sans dépendre du réseau.

**Pourquoi ce n’est pas “comme en Python avec `import yfinance` ?**
- En C++, il n’existe pas de bibliothèque standard équivalente « prête à l’emploi » intégrée au langage.
- Ici, le code choisit une implémentation bas niveau WinHTTP (`#ifdef _WIN32`) ; cela impose plus de code “système”, mais donne un contrôle fin et zéro dépendance Python.
- C’est un vrai point de difficulté du projet (voir section 4 et focus section 5).

### 3.4 `main.cpp` (CLI)

**Rôle**
- Démonstrateur console complet :
  - ajout manuel,
  - ajout via Yahoo,
  - retrait,
  - affichage,
  - calcul mu/vol avec matrice saisie,
  - fusion de portefeuilles,
  - corrélation automatique Yahoo.

**Pourquoi le garder ?**
- C’est la preuve d’usage demandée par la consigne (“toujours écrire un programme principal”).
- La CLI reste le mode le plus simple pour valider rapidement les fonctionnalités core.

### 3.5 `mainUI.cpp`

**Rôle**
- Fournir une interface web locale (serveur HTTP embarqué) pour manipuler le portefeuille visuellement.

**Pourquoi un deuxième point d’entrée ?**
- `main.cpp` répond à la consigne académique.
- `mainUI.cpp` prépare une logique “produit” plus conviviale.
- Séparer CLI et UI évite de mélanger les couches et simplifie le débogage.

### 3.6 `httplib.h`

**Rôle**
- Bibliothèque header-only utilisée pour l’interface web locale.

**Pourquoi ce choix ?**
- Très pratique en C++ : un seul header, pas d’installation lourde.
- Idéal dans un projet pédagogique/portable.

### 3.7 `tests/asset_portfolio_tests.cpp`

**Rôle**
- Vérifier automatiquement les invariants de `Asset` et `Portfolio`.

**Pourquoi un exécutable de test custom ?**
- Simplicité maximale (pas de framework externe requis).
- Convient à un environnement académique où il faut rester léger en dépendances.

### 3.8 `tools/build_cli.ps1`, `tools/build_ui.ps1`, `tools/test.ps1`

**Rôle**
- Scripts de build/test reproductibles (surtout sous Windows/PowerShell).

**Pourquoi des scripts séparés ?**
- Un script = un objectif (CLI, UI, tests), donc maintenance plus claire.
- Les options de compilation système (`-lwinhttp`, `-lws2_32`) sont centralisées.

---

## 4) Problèmes rencontrés et comment ils ont été surmontés

### 4.1 Récupération de données de marché en C++

**Problème**
- Contrairement à Python (`yfinance`), C++ n’offre pas un package standard simple et immédiat pour Yahoo.

**Solution adoptée**
- Implémentation réseau directe via WinHTTP (`Yahoo.cpp`).
- Parsing JSON ciblé des champs utiles (closes), puis transformation statistique locale.

**Compromis**
- Plus verbeux et plus technique qu’en Python.
- Mais dépendances maîtrisées et compréhension fine de la chaîne de traitement.

---

## 5) Focus particulier : difficulté “pas de yfinance natif en C++”

C’est probablement la difficulté la plus formatrice du projet.

En Python, on peut écrire en quelques lignes : `import yfinance as yf`, télécharger un DataFrame, puis enchaîner avec `pandas`/`numpy`.

En C++, il faut traiter plusieurs couches :

1. **Transport HTTP** (ou HTTPS) : ouverture de session, requête, réception.
2. **Interop système** : APIs Windows (`WinHttp*`), types wide strings.
3. **Parsing des données** : extraction des champs utiles depuis le JSON.
4. **Nettoyage** : ignorer `null`, filtrer prix invalides.
5. **Statistiques** : log-returns, moyennes/variances, annualisation.
6. **Gestion d’erreurs** : réseau, format inattendu, données insuffisantes.

Cette difficulté a été surmontée en construisant un module `Yahoo` isolé. Ce choix évite de “polluer” la logique `Portfolio` avec des détails réseau. Le cœur métier reste propre et testable même hors connexion.

---

## 6) Conclusion et pistes d’extension

### 6.1 Ce que le projet apporte déjà

- Base solide orientée objet pour un portefeuille multi-actifs.
- Calculs quantitatifs essentiels (rendement, risque, corrélation).
- API claire avec exceptions cohérentes.
- Double interface (CLI + UI locale).
- Tests unitaires reproductibles.

### 6.2 Comment aller plus loin

1. **Portabilité** : abstraction de la couche HTTP (WinHTTP + alternative Linux/macOS).
2. **JSON robuste** : parser JSON dédié (plutôt qu’extraction textuelle ciblée).
3. **Persistance** : sauvegarde/chargement du portefeuille (JSON/CSV/SQLite).
4. **Optimisation de portefeuille** : min-var, max-Sharpe, contraintes, frontier propre.
5. **Backtesting** : simulation historique avec frais, rebalancing, slippage.
6. **Qualité logicielle** : CMake, CI, tests d’intégration réseau mockés.

### 6.3 Intégration dans un cadre plus grand

Le projet peut devenir le noyau d’un outil d’aide à la décision :

- **côté data** : ingestion multi-sources (Yahoo + brokers + fichiers internes),
- **côté analytics** : moteur de risque/rendement et scénarios,
- **côté interface** : tableau de bord web pour analyste/étudiant,
- **côté gouvernance** : journalisation des hypothèses et reproductibilité des calculs.

En bref : ce projet est une base pédagogique pertinente qui peut évoluer vers une architecture quasi-professionnelle en conservant la séparation clé entre **modèle métier**, **acquisition de données** et **présentation**.
