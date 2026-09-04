# Omega Stick v1: claims and compliance constraints

Supporting document to [PRD.md](PRD.md). These are constraints on **what the
project may say**, which is why they belong alongside the product spec rather
than in a separate legal file.

**Not legal advice.** Full sourcing in
[research/04](research/04-regulatory-funding-compliance.md).

---

## 1. Prohibited claims

Omega Stick v1 must be described **only** as a computer and gaming input
peripheral for accessibility. Specifically prohibited:

- Any **therapeutic, diagnostic, rehabilitative, or treatment** claim.
- Any claim positioning it as a **communication device for people with medical
  impairments**.
- Any **wheelchair drive-control** capability.
- **"ADA compliant."** The ADA creates obligations for covered entities, not a
  product certification. There is no such mark for a peripheral.

### Why this is a hard constraint, not a style preference

The closest FDA classification for a medically-claimed device of this type is
**21 CFR 890.3710, "Powered communication system," product code ILQ, Class II**.

| Attribute | Value |
| --- | --- |
| 510(k) | **EXEMPT** |
| GMP / 21 CFR 820 | **NOT exempt** |

> The exposure is therefore **not** a premarket submission. It is establishment
> registration, device listing, a **21 CFR 820 quality system**, MDR reporting,
> and controlled labeling.

**Intended use as expressed in marketing is what triggers this.** A single
therapeutic claim on a product page changes the regulatory posture of the whole
project. Maintain a documented prohibited-claims list subject to marketing
review.

### The wheelchair line

Powered wheelchairs are Class II under **21 CFR 890.3860 and are 510(k)-
REQUIRED**. A drive-control input is a safety-critical component of that system.
This is why wheelchair drive control is a hard non-goal, not a deferred feature.

---

## 2. Compliance obligations for v1 as scoped

Wired, USB bus-powered, no radio, no battery:

| Region | Obligation |
| --- | --- |
| **US** | FCC Part 15 Subpart B, Class B, authorized by **SDoC**. Requires a US responsible party, a compliance information statement, and labeling |
| **EU** | EMC Directive 2014/30/EU, RoHS, REACH, WEEE producer registration. **RED does not apply** without a radio |
| **UK** | Accepts CE indefinitely (2024 extension) |

### Not applicable at this scope

RED, UN 38.3, IEC 62133-2, and modular radio integration paperwork.

> **All of these arrive the moment BLE or a battery ships.** That is the main
> practical reason for decisions D1 and HW-5.

### USB identifiers

| Option | Cost |
| --- | --- |
| USB-IF VID | **$6,000**, sharing forbidden |
| **Espressif VID `0x303A`** | **Free**, PID allocated by pull request |
| pid.codes | Free, fallback |

### If BLE ever ships

FCC clarified that open-source firmware is permitted **as long as it cannot
alter RF operating characteristics**. Architect the firmware now with a
separated RF/config layer so a future BLE variant does not force the project
closed.

---

## 3. Standards to cite

| Standard | Use |
| --- | --- |
| **ISO 9241-410** | The framework for the HW-2 force and throw measurement protocol. Explicitly covers joysticks |
| **ISO 9999** | Assistive product class, for tender and catalog documentation |
| **Section 508 Chapter 4 (Hardware)** and **EN 301 549** | Publish a **VPAT / Accessibility Conformance Report** |

> Institutional and government buyers ask for a VPAT, and **its absence blocks
> procurement outright.** EN 301 549, unlike WCAG, contains substantial hardware
> requirements and is what buyers cite for physical products.

---

## 4. Open source

- **Keep MIT** for firmware. Obligations are minimal and fully compatible with
  selling hardware.
- **Audit and preserve upstream third-party NOTICE files** before shipping
  binaries. ESP-IDF components are largely Apache-2.0.
- **Pursue OSHWA certification.** Free, self-certifying, annually renewed. The
  TMAG5273 has a public TI datasheet, so the third-party documentation condition
  is satisfiable. It is a credible trust signal to AT funders and the disability
  community, and the nearest competitor (Willow) already holds one.

**No conflict** was found between open sourcing and the FDA, FCC, or CE regimes
as v1 is scoped. Two friction points only:

1. If BLE ships, RF parameters must not be user-modifiable.
2. If the product ever became an FDA-listed Class II device, 21 CFR 820 design
   controls would require documented change control and a design history file,
   so community pull requests would need a gated release process feeding a
   controlled build.

---

## 5. Funding pathways

**Do not build the business case on Medicare.** General-purpose computer access
fails the "primarily and customarily used to serve a medical purpose" test, and
devices that can be unlocked for general computing are coded **A9270,
non-covered**.

### Realistic channels for v1

| Channel | Notes |
| --- | --- |
| **State AT Act programs** | ACL-funded: device loan, reuse, demonstration |
| **Vocational Rehabilitation** | Funds AT that enables employment |
| **IDEA / IEP** | District must fund AT written into the IEP, **at no cost to the family** |
| **VA** | Assistive technology and PSAS programs; VA medical centers have received Xbox Adaptive Controllers |
| **AbleGamers** | Supplies equipment free after an expert consult, though applications have been paused for demand |

### The SGD accessory route, and why it conflicts

If pursued, the code is **E2599** (accessory for a speech generating device, not
otherwise classified). It requires pairing with a covered **dedicated** SGD plus
an SLP evaluation naming the specific access method.

> **This channel and general-purpose positioning are mutually exclusive.**
> Marketing "it's also a general computer and gaming device" is exactly the
> language that pushes items to A9270 non-covered. **One SKU cannot do both.**
> v1 chooses general-purpose positioning; E2599 is out of scope.

### Funder documentation as a deliverable

Whatever the channel, funders typically require a physician face-to-face note,
an SLP or clinician evaluation naming the access method and why standard input
cannot be used, a written order prior to delivery, and per-item medical-necessity
justification.

Treat the documentation package as a product deliverable: a clinician-facing
evaluation template, a letter-of-medical-necessity template, and a spec sheet
quantifying actuation force and throw (which HW-2 produces anyway).
