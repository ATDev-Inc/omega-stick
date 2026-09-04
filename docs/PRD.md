# Omega Stick: Product Requirements Document

| Field | Value |
| --- | --- |
| **Version** | v1 |
| **Date** | 2026-09-04 |
| **Status** | Draft for review |
| **Product** | Low-force accessible joystick for computer control and gaming |
| **Repository** | `ATDev-Inc/omega-stick` |

**Supporting documents:** [requirements](requirements.md) (full specification) ·
[decisions](decisions.md) · [compliance](compliance.md) · [research](research/)

---

## 1. Product summary

Omega Stick is a joystick whose actuation force is adjustable across roughly 5
to 25 grams and defaults to under 10, intended for people who cannot operate a
conventional spring-centred joystick. It connects by USB, presents as a standard
input device requiring no installed software, and works across computers,
tablets, and games consoles.

The device is open source and **designed to be 3D printed**, so that it can be
produced locally by users, families, schools, and maker networks rather than
obtained solely through clinical supply channels.

## 2. Problem statement

Conventional joysticks place a spring between the user and the input. Standard
wheelchair joysticks require **200 to 336 grams of force** at full deflection.

| Evidence | Finding |
| --- | --- |
| Measured pinch strength, DMD and SMA adults | **1.4 to 1.6 kg**, approximately 20% of predicted |
| Pointing task error rates, users with physical disabilities | **Above 10% for 14 of 20 participants; 55% of drag tasks failed** |

The requirement addresses sustained use, not single actuation.

## 3. Target users

**Primary.** People with reduced strength, range of motion, or endurance arising
from SMA, ALS, muscular dystrophy, cerebral palsy, spinal cord injury (notably
C4 to C6), multiple sclerosis, arthritis, or essential tremor.

**Secondary.** Occupational therapists and assistive technology professionals
specifying equipment; caregivers and family members mounting and maintaining it;
maker networks and volunteer fabricators building units for individuals.

**Market sizing.** Approximately **250,000 people in the US cannot operate a
powered wheelchair**, with researchers estimating **half could with improved input
technology**. Computer use falls from 85% among people without disability to
**63% among those with severe dexterity impairment**. Approximately **46 million
US gamers have a disability**. Globally, **2.5 billion people require an assistive
product and approximately 1 billion have no access**, ranging from 90% access in
high-income countries to **3% in low-income countries**.
([evidence](research/02-user-needs-and-evidence.md))

## 4. Strategic rationale: open source

Open source is a product requirement, not a licensing preference. It addresses
four documented failure modes in this market.

| Failure mode | Evidence | How open source addresses it |
| --- | --- | --- |
| **Vendor exit strands users** | Ability Drive defunct; Traxsys exited AT; Switch-It MicroPilot discontinued (the only sub-10 g zero-throw device); QuadStick sold out on weekly allocation; mo-vis and Celtic Magic quote-only | Published designs and firmware outlive the originating organisation. Users retain the ability to repair, rebuild, and adapt independently |
| **Device abandonment** | **29% of assistive devices abandoned**, mobility aids highest. Leading predictors: user opinion not considered at selection, poor performance, changing user needs | Open development admits users as contributors; published spares and printable parts address changing needs and wear |
| **Access is gated** | Sub-10 g devices are dealer-gated, requiring a wheelchair, clinician, and insurance authorisation; one is approximately $3,300. Two-thirds of AT users worldwide self-fund | Local fabrication removes the supply chain as a gate. A printed unit reaches users no distribution channel serves |
| **No product fits every body** | Existing devices optimise for a median user that does not exist in this population | Users, therapists, and local makers modify the design directly rather than requesting a vendor roadmap change |

**Competitive note.** No commercial assistive technology joystick publishes
schematics or firmware. QuadStick publishes 3D files only; Celtic Magic reserves
all rights. Open source is therefore differentiating in this category, though not
unique: Willow, from a volunteer maker network, is open and certified.

**Commitments.** Permissive firmware licence retained; **OSHWA certification**
pursued (free, and the recognised trust signal for this audience); mechanical
files published.

## 5. Market positioning

**Actuation force alone is not differentiating.** Sub-10 gram devices already
ship: mo-vis Micro (8.5 g), ASL Molecule (8 g), Celtic Magic Feather (adjustable
to 5 g). All are closed, predominantly dealer-gated and unpriced, and several
operate only through proprietary wheelchair control systems.

**The unoccupied position is the combination:**

> Adjustable **5 to 25 grams** defaulting below 10 · functions as **both game
> controller and mouse** · Windows, macOS, **Linux**, Android, and Xbox ·
> **open source and 3D printable** · **published price below approximately $200** ·
> **published force-measurement method**

No vendor in this category publishes a force-measurement method, and independent
measurements contradict vendor claims where both exist.
([landscape](research/01-competitive-landscape.md))

## 6. Goals

| # | Goal | Acceptance criterion |
| --- | --- | --- |
| G1 | Immediate usability | Functional within 60 seconds of connection, with no installation, account, or clinical appointment |
| G2 | Dual output modes | Operates as game controller or mouse, selected on a host and retained by the device |
| G3 | Broad host support | Driverless operation on Windows, macOS, Linux, ChromeOS, Android; Xbox via the Xbox Adaptive Controller |
| G4 | Adjustable to the individual | All body-dependent parameters configurable and persisted to the device |
| G5 | Visible failure | Fault states distinguishable by a non-technical user with no console attached |
| G6 | Reproducible | Rebuildable from published firmware, mechanical files, and force-measurement method |

## 7. Scope

**In scope for v1:** USB-connected operation; game controller and mouse output;
per-user calibration and response tuning; host-based configuration with on-device
persistence; 3D-printable enclosure; published measurement method.

**Out of scope for v1:** wheelchair drive control; direct console connection;
wireless operation; any installed application as a dependency; medical claims;
head, eye, sip-and-puff, or voice input.
([rationale](decisions.md#out-of-scope-for-v1))

## 8. User stories

| As a | I need to | So that |
| --- | --- | --- |
| User with limited strength | Move the stick without pushing against a spring | I can use a computer for a full working session without exhaustion |
| User with restricted range of motion | Have the device learn my reachable limits | I get full cursor range from the movement I actually have |
| User with tremor | Adjust or disable filtering and response curves | The device follows my intent rather than a vendor's assumption about tremor |
| User on a managed school or work machine | Use it with nothing installed | I am not blocked by IT policy from using my own access device |
| Console gamer | Connect through the Xbox Adaptive Controller | I can play without licensed-accessory restrictions |
| Caregiver | Reposition and remount it quickly | It works at a desk, on a wheelchair tray, and in bed |
| Maker or family member | Print and assemble it from published files | Someone can have one without a supply chain or funding approval |
| Therapist | Cite a documented force specification | I can justify it in an assessment or funding request |

## 9. Requirements summary

Full specification, host compatibility matrix, and known defects:
**[requirements.md](requirements.md).**

| Area | Summary |
| --- | --- |
| **Sensing** | Contactless magnetic sensing, sampled fast enough that latency is imperceptible; safe behaviour on sensor loss |
| **Calibration** | Per-user centre and reach capture; automatic drift correction; on-device invocation with no host required |
| **Response** | Configurable dead zone, response curve, per-axis gain, inversion; tremor filtering supplied but disabled by default |
| **Output** | Game controller and mouse; driverless on all target hosts; Xbox via the Adaptive Controller |
| **Configuration** | Host-based, installing nothing; all settings persisted to the device so they travel with it |
| **Hardware contract** | Adjustable 5 to 25 gf; standard mounting interfaces; bus-powered, no battery; **design tolerant of 3D print variance** |

## 10. Success metrics

| Metric | Target | Rationale |
| --- | --- | --- |
| Time from connection to pointer control, unaided | Under 60 seconds | Complexity is a leading abandonment driver |
| Users completing calibration without assistance | 80% | Removes clinical dependency |
| **Continuous use before reported fatigue** | **2 hours** | **Primary abandonment driver in this population** |
| Sessions requiring manual recalibration | Under 5% | Device performance predictor |
| Target hosts working without drivers | 5 of 6 | |
| **Users with disabilities providing feedback before v1 release** | **10 minimum** | **Strongest single abandonment predictor is that the user was not consulted** |
| Independent successful builds from published files | 3 minimum | Validates the open-source distribution claim |

## 11. Constraints

Omega Stick v1 is positioned exclusively as a computer and gaming input
peripheral for accessibility. **Prohibited:** therapeutic, diagnostic, or
rehabilitative claims; wheelchair drive-control capability; the term "ADA
compliant."

How the product describes itself is what determines the category it is treated
as, so these are product constraints rather than marketing preferences. Details:
[compliance.md](compliance.md).
