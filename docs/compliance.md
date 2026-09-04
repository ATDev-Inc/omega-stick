# Omega Stick v1: claims and compliance constraints

Supporting document to [PRD.md](PRD.md). These are constraints on **what the
project may say**, which is why they belong alongside the product spec rather
than in a separate legal file.

**Not legal advice, and not a regulatory analysis.** This records what the
project will and will not claim about itself. Supporting research on funding and
product compliance is in [research/04](research/04-funding-and-compliance.md).

**Point-in-time snapshot.** Compiled during v1 planning and not actively
maintained. Re-check anything here before relying on it.

---

## 1. Prohibited claims

Omega Stick v1 must be described **only** as a computer and gaming input
peripheral for accessibility. Specifically prohibited:

- Any **therapeutic, diagnostic, rehabilitative, or treatment** claim.
- Any claim positioning it as a **communication device for people with medical
  impairments**.
- Any **wheelchair drive-control** capability.
- **"ADA compliant."** The ADA creates obligations for covered entities rather
  than a product certification, so there is no such mark for a peripheral to
  carry. (Inference from how the statute is structured, UNVERIFIED.)

### Why this is a hard constraint, not a style preference

Medical, therapeutic, and communication-device claims carry obligations that a
general-purpose input peripheral does not. How a product describes itself is
what determines which category it lands in, so the wording is a product
constraint rather than a matter of marketing taste.

This project has taken no regulatory advice and nothing here substitutes for it.
If the scope ever moves toward a medical or communication device, that needs
qualified counsel before anything ships.

Keep a documented prohibited-claims list and review published copy against it.

### The wheelchair line

Wheelchair drive control is a hard non-goal, not a deferred feature. A drive
input is a safety-critical part of a mobility system, which is a different
product category carrying a different set of obligations.

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

**No conflict** was found between open sourcing and the compliance regimes that
apply to v1 as scoped. Two friction points only:

1. If BLE ships, RF parameters must not be user-modifiable.
2. If the product scope ever changed such that a formal quality system applied,
   community pull requests would need a gated release process feeding a
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
