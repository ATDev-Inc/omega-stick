# 04: Regulatory, funding, and compliance

Captured 2026-09-04. See [README](README.md) for method and caveats.

**Not legal advice.** This records what primary sources say. Inferences are
marked UNVERIFIED.

**Question asked:** Is this a regulated medical device? How does it get funded?
What compliance obligations attach to selling it? What accessibility standards
will buyers cite? Does open sourcing conflict with any of it?

---

## 1. FDA and medical device status

### The trigger is intended use, not technology

A device is regulated if its labeling or marketing presents it for diagnosis,
cure, mitigation, treatment, or prevention of disease, or to affect structure or
function. **Technology choice does not determine this. Marketing language does.**

### The closest classification if medical claims are made

**21 CFR 890.3710, "Powered communication system"**: an AC- or battery-powered
device intended for medical purposes, used by persons unable to use normal
communication methods because of physical impairment.

| Attribute | Value |
| --- | --- |
| Product code | **ILQ** |
| Class | **II** |
| 510(k) | **EXEMPT** |
| GMP / 21 CFR 820 | **NOT exempt** |

Sources:
[FDA classification database, ILQ](https://www.accessdata.fda.gov/scripts/cdrh/cfdocs/cfpcd/classification.cfm?start_search=1&productcode=ILQ),
[21 CFR 890.3710](https://www.law.cornell.edu/cfr/text/21/890.3710)

> **The critical finding.** The worst case is not a 510(k) submission. It is
> **establishment registration + device listing + 21 CFR 820 quality system +
> MDR reporting + controlled labeling.** That is still a real and ongoing cost.
> 510(k) exemptions apply only to device types listed by regulation; other
> obligations survive the exemption
> ([FDA premarket notification](https://www.fda.gov/medical-devices/premarket-submissions-selecting-and-preparing-correct-submission/premarket-notification-510k)).

### The exemption can be voided

**21 CFR 890.9** voids the exemption if the device has a *different intended use*
from legally marketed devices of that type, or uses a *different fundamental
scientific technology*
([21 CFR 890.9](https://www.law.cornell.edu/cfr/text/21/890.9)).

Whether a Hall-effect magnetic sensor counts as a different *fundamental*
technology versus resistive joysticks is arguably no, but **UNVERIFIED**.

### General Wellness policy

The revised guidance (issued 6 January 2026) is the enforcement-discretion route
for low-risk products with wellness-only intended use
([FDA General Wellness](https://www.fda.gov/regulatory-information/search-fda-guidance-documents/general-wellness-policy-low-risk-devices)).
It is a **weaker fit than simply not making medical claims at all**.

### Wheelchair driving changes the answer materially

Powered wheelchairs are **Class II under 21 CFR 890.3860 and are 510(k)-
REQUIRED**, with dedicated FDA guidance
([21 CFR 890.3860](https://www.ecfr.gov/current/title-21/chapter-I/subchapter-H/part-890/subpart-D/section-890.3860),
[FDA wheelchair 510(k) guidance](https://www.fda.gov/regulatory-information/search-fda-guidance-documents/guidance-document-preparation-premarket-notification-510k-applications-mechanical-and-powered)).
A drive-control input is a safety-critical component of that system.

> This is a category change, not a feature. It is why wheelchair drive control is
> a hard non-goal for v1.

### Comparables

QuadStick and the Xbox Adaptive Controller appear to be positioned as consumer
and accessibility peripherals with no FDA clearance. **UNVERIFIED**: no FDA
registration or 510(k) record surfaced for either.

### What this means

v1 must be labeled and marketed **strictly as a computer and gaming input
peripheral for accessibility**, with no therapeutic, diagnostic, rehabilitative,
or communication-for-the-medically-impaired claims. Maintain a documented
prohibited-claims list subject to marketing review.

---

## 2. Reimbursement and funding (US)

### Medicare will not fund general computer access

DME must be durable, home-use, and **primarily and customarily used to serve a
medical purpose**. General-purpose computing fails this test
([Center for Medicare Advocacy](https://medicareadvocacy.org/medicare-info/medicare-part-b/durable-medical-equipment/)).

### The one viable code

**E2599, accessory for a speech generating device, not otherwise classified.**

Coverage exists when the base SGD is covered and each accessory's medical
necessity is documented in the SLP's formal evaluation. Access devices explicitly
named include **joysticks, switches, optical head pointers, wheelchair
integration devices, and scanning devices**
([CMS SGD article](https://www.cms.gov/medicare-coverage-database/view/article.aspx?articleId=52469),
[ASHA SGD policy](https://www.asha.org/practice/reimbursement/medicare/sgd_policy/)).

### The dedicated-device trap

| Code | Meaning |
| --- | --- |
| **E2510** | The SGD base unit itself (synthesized speech, multiple message formulation *and* multiple access methods). Not the joystick |
| **E2511** | SGD software alone |
| **A9270** | **Non-covered.** Laptops, tablets, general computers |
| **E2351** | Power wheelchair electronic interface to operate an **SGD** via the chair's control interface (covered). An interface to run lights or other electrical devices is A9270 non-covered |
| **K0108** | Wheelchair component or accessory not otherwise specified, billed one line per item |

Sources: [CMS LCD 33739](https://www.cms.gov/medicare-coverage-database/view/lcd.aspx?LCDId=33739),
[Noridian PMDs](https://med.noridianmedicare.com/web/jddme/dmepos/pmds),
[Noridian K0108](https://med.noridianmedicare.com/web/jddme/dmepos/manual-wheelchairs/billing-hcpcs-code-k0108-wheelchair-component-or-accessory-not-otherwise-specified)

> **Devices that can be "unlocked" or expanded to general computing are
> non-dedicated and not covered.** This is directly in tension with marketing the
> product as a general computer and gaming peripheral.

### Steve Gleason Act of 2015 (PL 114-40)

Removed SGDs from capped rental (making them purchases) and added coverage of
eye-tracking and gaze accessories from 1 January 2016
([congress.gov](https://www.congress.gov/bill/114th-congress/house-bill/628/text)).

### Non-Medicare routes, which are the realistic ones

| Route | Notes |
| --- | --- |
| **State AT Act programs** | ACL-funded: device loan, reuse, demonstration ([acl.gov](https://acl.gov/programs/assistive-technology/assistive-technology)) |
| **Vocational Rehabilitation** | Pays for AT that enables employment |
| **IDEA / IEP** | District must fund AT written into the IEP, **at no cost to the family** ([ATIA funding guide](https://www.atia.org/home/at-resources/what-is-at/resources-funding-guide/), [Parent Center Hub](https://www.parentcenterhub.org/ata/)) |
| **VA** | Assistive technology and PSAS programs. VA medical centers have received Xbox Adaptive Controllers ([VA AT program](https://www.va.gov/tampa-health-care/programs/assistive-technology-program/)) |
| **AbleGamers** | Grant program supplies equipment free after an expert consult, though applications have been paused for demand ([ablegamers.org](https://ablegamers.org/adaptive-gaming-equipment/)) |
| SpecialEffect (UK), Tetra Society | Commonly cited peers. **UNVERIFIED**, no primary source retrieved |

### Typical funder documentation

Physician face-to-face note; SLP formal evaluation naming the specific access
method and why standard input cannot be used; written order prior to delivery;
per-accessory medical-necessity justification.

### What this means

Do not build the business case on Medicare. Treat the **documentation package as
a deliverable**: a clinician-facing evaluation template, a letter-of-medical-
necessity template, and a spec sheet quantifying actuation force and throw.

> **The E2599 channel and general-purpose gaming positioning are mutually
> exclusive.** Marketing "it's also a general computer and gaming device" is
> exactly the language that pushes items to A9270. One SKU cannot do both.

---

## 3. Electrical and product compliance

### USB-only, no radio (the v1 scope)

**US:** FCC **Part 15 Subpart B**, Class B digital device (personal computer
peripheral). Authorization via **SDoC**, which replaced Verification/DoC on
2 November 2017. Requires a US responsible party, a compliance information
statement, and labeling
([47 CFR Part 15 Subpart B](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-B),
[FCC equipment authorization](https://www.fcc.gov/general/equipment-authorization-procedures)).

**EU:** EMC Directive 2014/30/EU + RoHS + REACH + WEEE producer registration.

**UK:** The UK indefinitely extended recognition of CE marking (2024), so CE
generally suffices
([compliancetesting.com](https://compliancetesting.com/is-ce-certification-accepted-in-the-uk-for-electronics/)).

### What changes if BLE is enabled

The radio falls under **FCC Part 15 Subpart C**. A pre-certified module
(ESP32-S3-WROOM-1 has an FCC ID) covers the intentional radiator via modular
approval, **but the host still needs its own Part 15B testing and SDoC**, and you
must follow the module grantee's integration instructions and "Contains FCC ID:"
labeling
([FCC KDB](https://apps.fcc.gov/eas/comments/GetPublishedDocument.html?id=50&tn=916170),
[compliancetesting.com](https://compliancetesting.com/fcc-testing-certification-for-espressif-esp32-devices/)).

In the EU, **RED 2014/53/EU applies to the whole product** and subsumes EMC and
LVD. **Module pre-certification does not remove device-level assessment**
([compliancetesting.com](https://compliancetesting.com/ce-certification-for-espressif-esp32-devices/)).

### Battery

Bus-powered with no cell: **UN 38.3 and IEC 62133 do not apply.** Adding a
Li-ion cell triggers UN 38.3 (transport) and IEC 62133-2 (product safety)
([TÜV SÜD](https://www.tuvsud.com/en-us/industries/mobility-and-automotive/automotive-and-oem/automotive-testing-solutions/battery-testing/un-dot-38-3)).

### USB VID/PID

| Option | Cost |
| --- | --- |
| USB-IF VID | **$6,000**, and sharing it is forbidden |
| **pid.codes** | Free, for open-source hardware projects ([pid.codes](https://pid.codes/pidcodes/2015/04/03/welcome/)) |
| **Espressif VID 0x303A** | Free, PID allocated by pull request. Requires an Espressif chip with a USB interface. No endorsement implied ([espressif/usb-pids](https://github.com/espressif/usb-pids/blob/main/README.md)) |

A standard HID class device may need no custom PID at all. USB-IF
certification and logo use is a separate, optional program.

### Prop 65

Businesses with fewer than 10 employees are exempt from the warning requirement,
but upstream manufacturers and distributors remain obligated, and contractual
indemnities are common
([OEHHA](https://oehha.ca.gov/proposition-65/businesses-and-proposition-65)).

### Open firmware plus a radio

FCC's U-NII software-security rules require RF parameters be secured against
user modification. The FCC clarified that **open-source firmware is permitted as
long as it cannot alter RF operating characteristics**; TP-Link paid $200k and
agreed to support third-party firmware
([FCC DA-16-850A1](https://transition.fcc.gov/Daily_Releases/Daily_Business/2016/db0801/DA-16-850A1.pdf),
[EFF](https://www.eff.org/deeplinks/2016/08/fcc-settlement-requires-tp-link-support-3rd-party-firmware)).

Those specific rules target 5 GHz U-NII; direct applicability to 2.4 GHz BLE is
**UNVERIFIED**, but the governing principle (users must not be able to build
out-of-spec RF) is the risk to design around.

### What this means

Ship v1 **wired-only, USB bus-powered, no battery**. That reduces obligations to
FCC Part 15B SDoC plus CE (EMC/RoHS/REACH/WEEE), and avoids RED, UN 38.3, and
modular-integration paperwork entirely. Budget BLE as a separate
compliance-gated milestone. Use a free Espressif-VID PID. If BLE ever ships, the
firmware must lock RF parameters behind a non-user-modifiable layer even though
the rest stays open.

---

## 4. Accessibility standards buyers will cite

| Standard | Relevance |
| --- | --- |
| **Section 508 / 36 CFR Part 1194** | Applies to federal ICT procurement. Includes a **Chapter 4 Hardware** section: operable parts, standard connections, closed functionality, status indicators ([ecfr.gov](https://www.ecfr.gov/current/title-36/chapter-XI/part-1194)) |
| **EN 301 549** | The EU harmonized ICT accessibility standard. Unlike WCAG it contains substantial **hardware** requirements. This is what institutional buyers cite for hardware ([section508.gov](https://www.section508.gov/manage/laws-and-policies/international/)) |
| **ISO 9241-410:2008** | Ergonomic design criteria for physical input devices, **explicitly covering joysticks**. The natural technical basis for justifying a force specification ([iso.org](https://www.iso.org/standard/38899.html)) |
| **ISO 9999:2022** | Classifies assistive products. Used in tenders and catalogs ([iso.org](https://www.iso.org/standard/72464.html)) |
| **ADA** | Creates obligations for covered entities (employers, public accommodations), **not a product certification.** There is no "ADA-compliant" mark for a peripheral. Treat that phrasing as a marketing risk (inference, UNVERIFIED) |
| **EAA (Directive 2019/882)** | Applicable from 28 June 2025. Covers "consumer general purpose computer hardware systems and operating systems." Whether a stand-alone input peripheral is in scope is **UNVERIFIED** ([legislation.gov.uk](https://www.legislation.gov.uk/eudr/2019/882/body/2019-04-17/data.xht)) |

### What this means

Produce a **VPAT / Accessibility Conformance Report** covering Section 508
Chapter 4 and EN 301 549 hardware clauses. Institutional and government buyers
ask for it and **its absence blocks procurement outright**. Cite ISO 9241-410 in
the spec sheet as the framework for force and travel measurements. List an
ISO 9999 class in tender documentation. **Never claim "ADA compliant."**

---

## 5. Open source hardware and firmware

### Licensing

The repository is MIT, with dual copyright (esp-cpp 2023 + ATDev Inc. 2026). MIT
obligations are minimal: retain the copyright notice and license text in
distributions. No copyleft, no source-disclosure trigger, **fully compatible with
selling hardware**.

Upstream ESP-IDF components carry their own (largely Apache-2.0) notices that
must be preserved. **Verify the full third-party NOTICE set before shipping
binaries.**

### OSHWA certification

**Free and self-certifying**, valid one year with annual renewal. Requires that
all creator contributions be open source, that third-party components have
publicly accessible and shareable datasheets, and that each product be
registered with links to documentation
([requirements](https://certification.oshwa.org/requirements.html),
[process](https://certification.oshwa.org/process/hardware.html)).

The TMAG5273 has a public TI datasheet, so that condition is satisfiable.

### Conflicts

**No conflict was found** between open sourcing and the FDA, FCC, or CE regimes.
Two friction points only:

1. If BLE ships, RF-parameter control must not be user-modifiable (section 3).
2. If the product ever becomes an FDA-listed Class II device, **21 CFR 820
   design controls require documented change control and a design history file**.
   Community pull requests would then need a gated release process feeding a
   controlled build.

### What this means

Keep MIT and pursue OSHWA certification: free, and a credible trust signal for
AT funders and the disability community (the nearest competitor, Willow, already
holds one). Add a third-party license and NOTICE audit before first shipment.
Architect the firmware now with a **separated RF/config layer** so a future BLE
variant does not force the project closed. If a regulated SKU is ever
contemplated, plan for a maintainer-controlled release branch distinct from the
open `main`.
