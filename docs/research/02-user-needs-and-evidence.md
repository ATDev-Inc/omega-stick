# 02: User needs and evidence base

Captured 2026-09-04. See [README](README.md) for method and caveats.

**Question asked:** Who needs a low-force joystick and how many are there? What
does the literature say about force, fatigue, and tremor? Why is assistive
technology abandoned? How do these devices actually get mounted?

---

## 1. Who needs this, and how many

### Neuromuscular and neurological populations (US unless noted)

| Condition | Figure | Source |
| --- | --- | --- |
| SMA | ~9,400 living (Type I 1,610; II 3,944; III 3,875). Birth incidence 1:6,000 to 1:10,000; newborn screening 1:14,694 | [PMC5704427](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC5704427/), [PMC11250364](https://pmc.ncbi.nlm.nih.gov/articles/PMC11250364/) |
| ALS | ~33,000 cases (2022), projected >36,000 by 2030. 2017 capture-recapture: 24,821 (7.7 per 100,000) | [CDC National ALS Registry](https://www.cdc.gov/als/php/abstracts-publications-reports/prevalence-2022-2030.html), [PMC9568617](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC9568617/) |
| Duchenne / Becker MD | 1.38 per 10,000 males aged 5 to 24 (2010); 1.47 per 10,000 (2007) | MD STARnet, [PMC10031951](https://pmc.ncbi.nlm.nih.gov/articles/PMC10031951/) |
| Spinal cord injury | ~296,000 living (range 252,000 to 373,000); ~17,900 new per year. **C4 = 15.7% and C5 = 14.8% of all new injuries** | [NSCISC Facts and Figures](https://msktc.org/sites/default/files/Facts-and-Figures-2025-Eng-508.pdf) |
| Cerebral palsy | 1 in 345 children (ADDM 2010); >764,000 children and adults | [CDC ADDM](https://archive.cdc.gov/www_cdc_gov/ncbddd/cp/data.html) |
| Multiple sclerosis | ~1,000,000 (twice the prior estimate) | Wallin et al., *Neurology*, 2019 |
| Essential tremor | 6.4M adults at 2.6% prevalence; claims-diagnosed 1.1M (0.42%) | [Neurology 94(15) suppl](https://www.neurology.org/doi/10.1212/WNL.94.15_supplement.4458), [Tremor J. 2025](https://tremorjournal.org/articles/10.5334/tohm.1060) |
| Arthritis | 53.2M adults (21.2%), 2019 to 2021; **25.7M with activity limitation** | [CDC MMWR 72(41)](https://www.cdc.gov/mmwr/volumes/72/wr/mm7241a1.htm) |
| Arthrogryposis | **UNVERIFIED**, no reliable US prevalence retrieved | |

### The most on-point figure found

> **~250,000 people in the US cannot operate a powered wheelchair** due to
> motor, sensory, or cognitive impairment. The authors estimate **about half
> could drive if better input technology existed.**

Dicianno et al., *Joystick Control for Powered Mobility*, PM&R 2009,
[PMC2788504](https://pmc.ncbi.nlm.nih.gov/articles/PMC2788504/)

Note that this figure describes wheelchair *driving*, which the PRD scopes out
for regulatory reasons. It is cited as evidence that input force is a real and
quantified barrier, not as a target market for v1.

### Computer access specifically

- **Dexterity impairment among working-age computer users: 19% (24.4M) mild,
  5% (6.8M) severe.**
- **Computer use falls from 85% (no disability) to 80% (mild) to 63% (severe).**
  That 22-point gap is the addressable problem.
- Source: [PMC2788505](https://pmc.ncbi.nlm.nih.gov/articles/PMC2788505/)
- CDC DHDS: 28.7% of US adults report a functional disability; **mobility is the
  most common type at ~13%**
  ([CDC MMWR](https://www.cdc.gov/mmwr/preview/mmwrhtml/mm6429a2.htm))

### Gaming

- **46 million US gamers with disabilities** (AbleGamers estimate, carried in
  [ESA 2020 Essential Facts](https://www.theesa.com/wp-content/uploads/2021/03/Final-Edited-2020-ESA_Essential_facts.pdf))
- AbleGamers: people with disabilities are **51% more likely to be socially
  isolated**

### Global

- **2.5 billion people need at least one assistive product; ~1 billion are
  denied access.** Access ranges from 3% (low-income countries) to 90%
  (high-income). **Two-thirds pay out of pocket.** Need rises to 3.5 billion by
  2050.
- [WHO/UNICEF GReAT 2022](https://www.who.int/news/item/16-05-2022-almost-one-billion-children-and-adults-with-disabilities-and-older-persons-in-need-of-assistive-technology-denied-access--according-to-new-report)

---

## 2. Evidence on force and effort

### Force benchmarks

| Device class | Force |
| --- | --- |
| **Standard power-wheelchair joystick** | **200 to 250 gf** ([ATandMe](https://www.atandme.com/low-force-wheelchair-joysticks/)), 203 to 336 gf per NRRTS |
| mo-vis Micro Joystick | 8 gf, 3.3 mm throw (adapted version 5 gf) |
| ASL 130 MEC | 18 gf |
| Switch-It MICROPILOT | 10 gf, zero deflection |
| Switch-It MICROGUIDE | 40 gf |
| mo-vis Multi Joystick | 50 gf |
| **Xbox Adaptive Joystick** | **130 gf button actuation, 870 gf stick press.** 1/4-20 threaded mount, 106.2 g mass ([Xbox specs](https://www.xbox.com/en-US/accessories/controllers/xbox-adaptive-joystick)) |

Analog stick *deflection* force for mainstream gamepads: **UNVERIFIED**, no
vendor publishes it.

### Residual strength in target populations

Decostre, De Antonio, Servais & Hogrel, *Relationship Between Hand Strength and
Function in DMD and SMA*, J Neuromuscul Dis 2024, n=168
([PMC11307056](https://pmc.ncbi.nlm.nih.gov/articles/PMC11307056/)):

| Measure | DMD | SMA |
| --- | --- | --- |
| Mean handgrip | **4.08 kg** | **4.75 kg** |
| Mean key pinch | **1.40 kg** | **1.63 kg** (~20% of predicted) |
| Function retained down to | 11% predicted grip | 8% predicted grip |

**Implication:** a 200 to 250 gf (2.0 to 2.5 N) sustained demand is a
substantial and repeated fraction of residual pinch force for the weakest users.
A sub-10 gf target is roughly **1/25th** of that.

### Isometric vs displacement: the evidence is split, not settled

This is important. It is why the PRD makes response mode configurable (decision
D9) rather than committing to one architecture.

| Study | Finding |
| --- | --- |
| Cooper et al., *Analysis of position and isometric joysticks for powered wheelchair driving*, IEEE TBME 2000 ([PubMed 10916261](https://www.ncbi.nlm.nih.gov/pubmed/10916261)) | Isometric produced **fewer movement errors but longer reaction times** |
| Dicianno et al., *Joystick use for virtual EPW driving in individuals with tremor*, JRRD 2009;46(2):269-75 ([PubMed 19533540](https://pubmed.ncbi.nlm.nih.gov/19533540/)) | Isometric **preferred by participants with tremor** |
| Stewart, *Isometric Joystick: control by adolescents and young adults with cerebral palsy*, Aust Occup Ther J 1992 | Displacement was **superior for 4 of 5 CP subjects with prior displacement experience**. No difference for joystick-naive users |

Also relevant: isometric users **over-exert far beyond what is needed** without
training (PMC2788504).

### Tremor filtering: a documented negative result

- Low-pass filtering is inadequate. **MS tremor runs 3.5 to 5.0 Hz**, so a
  cutoff low enough to remove it introduces phase lag and deletes intentional
  commands.
- Adaptive notch (WFLC) is preferred in principle, but **Dicianno 2009 found
  WFLC did not improve performance** and may have damped intended motion.

**This is a real negative result, not an absence of evidence.** It is why the
PRD ships tremor filtering available but defaulted off, and user-defeatable
(decision D8).

### Fatigue and customization

| Finding | Source |
| --- | --- |
| Mouse and keyboard use draws only **~7% of wrist MVC**, yet **10 minutes at 15% MVC significantly degraded mouse tracking accuracy** | [PMC2788505](https://pmc.ncbi.nlm.nih.gov/articles/PMC2788505/) |
| Trewin & Pain: **error rates >10% for 14 of 20 users** with physical disabilities; **55% of drag tasks failed outright** | same |
| Pointer **gain adjustment gave a modest but statistically significant improvement (p<0.05, n=16)** | same |
| Automatic keyboard parameter configuration achieved a **96% reduction in repeated characters** | same |

**Two implications:** sustained low-level force is enough to impair fine motor
control even in unimpaired users, and *automatic* or guided configuration
outperforms manual tuning.

---

## 3. Assistive technology abandonment

This section drives the PRD's success metrics (Section 13).

| Study | Population | Non-use rate | Notes |
| --- | --- | --- | --- |
| **Phillips & Zhao 1993**, *Predictors of Assistive Technology Abandonment*, Assistive Technology 5(1):36-45, n=227 ([PubMed 10171664](https://pubmed.ncbi.nlm.nih.gov/10171664/)) | US | **29.3% completely abandoned** | **Mobility aids abandoned more than any other category.** Peaks in **year 1 and after year 5** |
| Federici & Borsci 2016 ([PubMed 26784731](https://pubmed.ncbi.nlm.nih.gov/26784731/)) | Italian NHS, n=749 | **17.9% non-use** | **40% of those never used the device at all** |
| Sugawara et al. 2018 ([PubMed 29334475](https://pubmed.ncbi.nlm.nih.gov/29334475/)) | São Paulo | **19.38% non-use** | Wheeled mobility had the *lowest* non-use; upper-limb orthoses the highest |

### The four significant predictors (Phillips & Zhao)

1. **User opinion not considered in selection**
2. Easy procurement
3. **Poor device performance**
4. Change in user needs

### Modern confirmation

A 2025 systematic review of AT for tetraplegia (34 studies, 366 participants,
[Assistive Technology](https://www.tandfonline.com/doi/full/10.1080/10400435.2025.2540119))
found that technically sound devices are abandoned when they give **poor UX,
operate slowly, are complex, cause high fatigue, or require excessive physical
effort**.

> The strongest documented predictor is that the user's opinion was not
> considered during selection. A project cannot retrofit that after shipping.
> This is why the PRD carries a process metric requiring feedback from real AT
> users before the v1 tag, rather than only product metrics.

---

## 4. What users complain about today

### Cost stacking

| Item | Price |
| --- | --- |
| Xbox Adaptive Controller base | **$99.99** |
| A *usable* XAC setup | **$270+** |
| Logitech Adaptive Gaming Kit | $99.99 |
| Individual switches | $22.55 to $549 |
| QuadStick FPS | from $549 |
| QuadStick Singleton | from $449 |
| Console + XAC setup | approaching **$1,000** |

Sources: [Can I Play That](https://caniplaythat.com/2020/09/24/xbox-adaptive-controller-review-xbox-pc/),
[Equal Entry](https://equalentry.com/does-the-xbox-adaptive-controller-deliver-on-the-hype/),
[QuadStick](https://www.quadstick.com/shop)

### Not usable standalone

The XAC ships with **8 buttons total**, and only **2 of 7 buttons plus the
d-pad** are usable without buying external hardware. Reviewers describe it as
designed for the most severely disabled while **leaving moderate-impairment
users unserved**.

### Configuration burden

Reviewers report **hours of manual remapping and emulator digging** before basic
function. Clinical drive-control tuning is therapist-mediated.

### Provisioning throughput

SpecialEffect: initial contact **2 to 4 weeks**, then a **1 to 3 month waiting
list** for a visit. **British Isles only.** The loan kit is recalled and
reissued. ([SpecialEffect FAQs](https://specialeffect.org.uk/faqs))

### Affordability

Two-thirds of AT users worldwide pay out of pocket (WHO 2022).

### UNVERIFIED

Drift, dead zone, and repairability complaints are widely discussed in community
forums, but primary community threads could not be retrieved (Reddit API
blocked). Wheelchair breakdown and repair statistics also **UNVERIFIED**.

---

## 5. Mounting and physical context

### De facto standards

| Standard | Dimensions | Notes |
| --- | --- | --- |
| **AMPS 4-hole pattern** | **30 mm x 38 mm** (1.181" x 1.496") | The industry standard, used by RAM Mounts. 2-hole variant is the diagonal at ~48.41 mm ([Bulletpoint](https://www.bulletpointmountingsolutions.com/pages/amps-standard-patterns-explained)) |
| **RAM ball sizes** | B = 1", C = 1.5", D = 2.25" | **The 1" B ball is the practical wheelchair and AT size** ([RAM Mounts](https://rammount.com/collections/amps-bases)) |
| **1/4"-20 UNC** | | **The strongest cross-ecosystem interface.** The Xbox Adaptive Joystick uses it; Rehadapt ships a Tube Ø10x50 with 1/4"-UNC thread |
| Special-controls hardware | 6 mm rod + half clamp | mo-vis ships a Q2M Rod D6 x 50 mm and D6 half clamp |

### The bridge to the clinical ecosystem

The **Rehadapt Universal Device Socket bridges to VESA 75/100, Daessy, Armon
Edero, and LC-Tech** ([Rehadapt](https://rehadapt.com/sockets/system-bridges/)).
Supporting UDS or VESA therefore reaches the whole clinical mount ecosystem
through one interface.

Daessy uses modular quick-release bases plus device adapter plates. Exact tube
OD **UNVERIFIED**.

### Real use contexts

Wheelchair armrest, tray, swing-away arm, chin or lip mount, plus **desk/table
and in-bed use**.

> Mounting must be repositionable by a caregiver in seconds, not bolted once.

---

## Top 6 requirements this evidence implies

These fed directly into the PRD.

1. **Actuation under 10 gf, with force and throw both specified and published.**
   Level with mo-vis Micro (8 gf) and Switch-It MICROPILOT (10 gf), and ~25x
   below the 200 to 250 gf clinical standard. Defensible against measured
   residual pinch forces of 1.4 to 1.6 kg in DMD and SMA.

2. **Ship both displacement and isometric-like modes, selectable in software.**
   The literature is genuinely split. Configurability, not an architecture
   choice, is the evidence-backed answer.

3. **User-facing customization with no clinician required**: deadzone, gain
   curve, per-axis scaling, tremor filtering, profiles. Gain tuning shows
   measured significant benefit, and configuration burden is a top complaint.
   Guided or automatic calibration beats manual tuning (96% error reduction
   precedent).

4. **Tremor handling must be adaptive and defeatable.** Fixed low-pass fails at
   MS tremor frequencies of 3.5 to 5.0 Hz, and WFLC did not help in the one
   controlled trial. Expose it, default it off, let users A/B it.

5. **Mount to the existing ecosystem out of the box**: 1/4"-20 insert + AMPS
   30x38 mm pattern, plus a 1" RAM ball adapter. That combination reaches RAM,
   Rehadapt UDS (and through it VESA 75/100 and Daessy), and the Xbox Adaptive
   Joystick convention, at negligible BOM cost.

6. **Design against the abandonment predictors directly**: involve users in
   selection, price far below the $270 to $549 adaptive band, and make it
   self-repairable. Open hardware plus published spares and firmware answers
   "changing needs" and repairability in a way no proprietary vendor currently
   does.
