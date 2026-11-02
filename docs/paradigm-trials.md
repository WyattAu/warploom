# Paradigm trials (551-850): implementation funnel

One row per paradigm. We work the **P0 queue first, one trial at a time**:
spike it small, measure against what the engine already has, then **adopt**
(land + docs) or **reject** (ADR entry in `docs/paradigm-adr.md` with the
measured reason). A trial that cannot be measured does not land.

## Protocol

1. Pick the next `pending` P0 item (queue order: **554 done -> 831 -> 608 ->
   611 -> 800**; P1s are staged after the P0 queue drains).
2. Spike: smallest test/probe that exercises the claim against the current
   implementation (unit test, probe binary, or capture A/B -- never a
   rewrite).
3. Measure: numbers from this codebase (correctness parity + perf). The
   paper's/list's claimed superiority is not this engine's superiority.
4. Verdict: adopt (land verified, or revert) or reject (ADR entry, status
   `rejected`, reason points at the ADR). Never weaken a check to pass.
5. P2 items need a consumer before they enter the queue; OUT items need a
   subsystem to exist first (review trigger in ADR-001).

## Tier semantics

- **P0** -- spike-ready: small, CPU- or test-side, maps to an existing
  subsystem with a clear incumbent to beat.
- **P1** -- relevant and valuable, but staged (needs design, infra, or a
  bigger build than a spike).
- **P2** -- adjacent/future: no current consumer; enters the queue only on
  consumer need.
- **OUT** -- out of scope for this repo (reason in ADR-001): no subsystem
  exists, or silicon/vendor/dependency constraints forbid it.

## Status values

`pending` (P0/P1/P2, untouched) · `in-progress` (exactly one) ·
`adopted` (landed, ADR ref) · `rejected` (ADR ref) · `out-of-scope` (ADR-001).

## Cluster L: radiative transfer, GI, wave optics (551-560)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 551 | Path-space symplectic radiative transfer | render | P2 | pending | -- |
| 552 | Wigner phase-space wave optics | render | P2 | pending | -- |
| 553 | Fredholm resolvent GI kernels | render | P2 | pending | -- |
| 554 | Plucker line-geometry ray-primitive intersectors | render | P0 | rejected | ADR-002 |
| 555 | Stokes-vector polarization manifolds | render | P2 | pending | -- |
| 556 | Green's-function subsurface scattering | render | P2 | pending | -- |
| 557 | Optimal-transport caustic inversion | render | P2 | pending | -- |
| 558 | Spherical wavelet frames, specular transport | render | P1 | pending | -- |
| 559 | Microfacet foliations on tangent bundles | render | P2 | pending | -- |
| 560 | BEM integral radiosity (FMM) | render | P2 | pending | -- |

## Cluster LI: real-time rasterization, shading (561-570)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 561 | Plucker-Grassmann continuous visibility (no Z-buffer) | render | OUT | out-of-scope | ADR-001 |
| 562 | Analytic visibility half-space integration (no MSAA/TAA) | render | P2 | pending | -- |
| 563 | Differential silhouette shadow fields (no shadow maps) | render | P2 | pending | -- |
| 564 | Geometric fiber-bundle screen-space shading (no G-buffer) | render | P2 | pending | -- |
| 565 | Conformal surface reflection mapping (no SSR) | render | P2 | pending | -- |
| 566 | Analytic spherical-triangle horizon clipping (area lights) | render | P1 | pending | -- |
| 567 | Homogeneous line-coordinate raster-free projection | render | OUT | out-of-scope | ADR-001 |
| 568 | Spectral transmittance volumetric Lie integrators | render | P2 | pending | -- |
| 569 | Phase-space micro-surface shaders (no metallic/rough) | render | P2 | pending | -- |
| 570 | Euler-topology transparency (no OIT) | render | P2 | pending | -- |

## Cluster LII: neural rendering, volumetric fields (571-580)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 571 | Radon-inverse radiance fields (no NeRF training) | render | OUT | out-of-scope | ADR-001 |
| 572 | Riemannian metric-tensor Gaussian covariance fields | render | P2 | pending | -- |
| 573 | Analytic level-set zero-crossing extraction | mesh | P2 | pending | -- |
| 574 | Eikonal geodesic field evaluators (no sphere tracing) | render | P2 | pending | -- |
| 575 | Poisson-Beltrami radiance compression | render | P2 | pending | -- |
| 576 | Helmholtz nodal-domain implicit geometry | asset | P2 | pending | -- |
| 577 | Lie-group SE(3) bundle adjustment | tool | P2 | pending | -- |
| 578 | Plucker-dual lumigraph field inversion | render | P2 | pending | -- |
| 579 | Morse-complex implicit stratification | mesh | P2 | pending | -- |
| 580 | Curvature-weighted variational surface reconstruction | mesh | P2 | pending | -- |

## Cluster LIII: CAD, B-Rep, geometric modeling (581-590)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 581 | Algebraic-geometric implicit spline schemes (no NURBS) | cad | P2 | pending | -- |
| 582 | Stratified semi-algebraic exact boundary topology | cad | P2 | pending | -- |
| 583 | CSG via homological intersection rings | cad | P2 | pending | -- |
| 584 | Cylindrical algebraic decomposition constraints | cad | P2 | pending | -- |
| 585 | Non-manifold cellular homology complex kernels | cad | P2 | pending | -- |
| 586 | Medial-axis exact Voronoi skeletal extraction | cad | P2 | pending | -- |
| 587 | Lie-algebraic parametric deformation (no topological naming) | cad | P1 | pending | -- |
| 588 | Differential-form surface blending operads (fillets) | cad | P2 | pending | -- |
| 589 | Semi-algebraic exact tolerance envelopes | cad | P2 | pending | -- |
| 590 | Functional representation (F-Rep) universal solid algebra | cad | P1 | pending | -- |

## Cluster LIV: mesh generation, discrete differential geometry (591-600)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 591 | DEC Hodge-star metric duality (no cotangent drift) | mesh | P1 | pending | -- |
| 592 | Conformal metric uniformization via Ricci flow (no UV unwrap) | mesh | P2 | pending | -- |
| 593 | Cross-field foliation pure-hex meshing | mesh | P2 | pending | -- |
| 594 | Optimal-transport centroidal Voronoi tessellation | mesh | P1 | pending | -- |
| 595 | Geodesic heat method on metric spaces | mesh | P1 | pending | -- |
| 596 | Curvature-aligned quad parametrization | mesh | P2 | pending | -- |
| 597 | Harmonic 1-form topological surgery | mesh | P2 | pending | -- |
| 598 | Intrinsic triangulations via Ptolemy flips | mesh | P2 | pending | -- |
| 599 | Functional-map spectral shape correspondence (no ICP) | mesh | P2 | pending | -- |
| 600 | Mean-curvature-flow surface regularization | mesh | P2 | pending | -- |

## Cluster LV: engine architecture, traversal (601-610)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 601 | Plucker beam-tracing foliations (no BVH) | render | P2 | pending | -- |
| 602 | Homological multi-scale geometry slicing (no LOD pop-in) | render | P1 | pending | -- |
| 603 | Dual-graph topological scene connectivity (no scene graph) | core | P2 | pending | -- |
| 604 | Hyperbolic metric world coordinates (no origin rebasing) | core | P2 | pending | -- |
| 605 | Homological occlusion shadow fields (no HZB readback) | render | P2 | pending | -- |
| 606 | Symplectic momentum schedulers (no ECS ordering stalls) | core | P1 | pending | -- |
| 607 | Topological cell-complex collision geometries | phys | P2 | pending | -- |
| 608 | Golden-ratio Clifford spatial hashing | phys | P0 | adopted | ADR-004 |
| 609 | Monadic capability scripting membranes (no Lua/bindings) | core | P2 | pending | -- |
| 610 | Deterministic lockstep phase-space netcode | net | OUT | out-of-scope | ADR-001 |

## Cluster LVI: character animation, deformation (611-620)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 611 | Lie-group SE(3) Riemannian blend skinning (no LBS collapse) | anim | P0 | adopted | ADR-005 |
| 612 | Hyperelastic muscle fiber potentials (no corrective shapes) | anim | P2 | pending | -- |
| 613 | Riemannian optimization on Lie chains (full-body IK) | anim | P2 | pending | -- |
| 614 | Koopman linearization of motion (no motion matching) | anim | P2 | pending | -- |
| 615 | Diffeomorphic surface morphing (no blendshape topology lock) | anim | P2 | pending | -- |
| 616 | Symplectic geodesic motion retargeting (no foot skate) | anim | P2 | pending | -- |
| 617 | Cosserat rod invariants for hair/fur | anim | P2 | pending | -- |
| 618 | Variational contact-aware momentum locomotion (no state machines) | anim | P2 | pending | -- |
| 619 | Featherstone articulated rigid bodies (no constraint drift) | phys | P1 | pending | -- |
| 620 | Lie-algebraic pose-graph interpolation (no SLERP) | anim | P2 | pending | -- |

## Cluster LVII: contact, collision, rigid bodies (621-630)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 621 | Spacetime Minkowski polytope sweeping (no tunneling) | phys | P2 | pending | -- |
| 622 | Second-order cone complementarity solvers (no PGS) | phys | P1 | pending | -- |
| 623 | Moreau sweeping process for nonsmooth contact | phys | P1 | pending | -- |
| 624 | Primal-dual interior-point rigid assembly | phys | P2 | pending | -- |
| 625 | Symplectic Lie-Poisson gyro integrators | phys | P2 | pending | -- |
| 626 | Analytic support-function hull tracking (no GJK/EPA) | phys | P1 | pending | -- |
| 627 | Nonsmooth variational impact integrators (no Baumgarte) | phys | P2 | pending | -- |
| 628 | Maximal-dissipation anisotropic friction | phys | P2 | pending | -- |
| 629 | Discrete-element contact topology lattices (granular) | phys | P2 | pending | -- |
| 630 | Exact complementarity pivoting on Delassus submatrices | phys | P1 | pending | -- |

## Cluster LVIII: soft bodies, cloth, FEM, XPBD (631-640)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 631 | Material-space metric hyperelasticity (no spring cloth) | phys | P2 | pending | -- |
| 632 | Variational ARAP global-local solvers | phys | P1 | pending | -- |
| 633 | Incremental Potential Contact barriers (no penetration) | phys | P1 | pending | -- |
| 634 | Projective dynamics via ADMM (no Newton divergence) | phys | P2 | pending | -- |
| 635 | Discrete-shells curvature energy (cloth bending) | phys | P1 | pending | -- |
| 636 | Dual-quaternion continuum torsion (no volume loss) | phys | P2 | pending | -- |
| 637 | Eigenvalue-domain strain-limiting projections | phys | P2 | pending | -- |
| 638 | XPBD exact compliance formulation (verify/adopt formally) | phys | P1 | pending | -- |
| 639 | Adaptive octree B-spline solids (no tet meshing) | phys | P2 | pending | -- |
| 640 | C2 barrier potentials with analytic Hessians | phys | P2 | pending | -- |

## Cluster LIX: continuum mechanics, fluids (641-650)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 641 | Hamiltonian vortex filaments / Hasimoto (no grid dissipation) | fluid | OUT | out-of-scope | ADR-001 |
| 642 | APIC on incompressible manifolds (no FLIP noise) | fluid | OUT | out-of-scope | ADR-001 |
| 643 | DEC incompressible Navier-Stokes (no MAC artifacts) | fluid | OUT | out-of-scope | ADR-001 |
| 644 | Geometric multigrid on dynamic octrees (no CG stalls) | fluid | OUT | out-of-scope | ADR-001 |
| 645 | MPM elasto-plasticity (sand/snow/mud/water unified) | fluid | OUT | out-of-scope | ADR-001 |
| 646 | Boundary kernel integrals for SPH | fluid | OUT | out-of-scope | ADR-001 |
| 647 | Level-set narrow-band Eikonal reinitialization | fluid | OUT | out-of-scope | ADR-001 |
| 648 | Cahn-Hilliard multiphase diffuse interfaces | fluid | OUT | out-of-scope | ADR-001 |
| 649 | Oldroyd-B log-conformation viscoelastic integrators | fluid | OUT | out-of-scope | ADR-001 |
| 650 | Phase-field Griffith fracture (no Voronoi shatter) | fluid | OUT | out-of-scope | ADR-001 |

## Cluster LX: inverse rendering, differentiable optics (651-660)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 651 | Path-space differentiable boundary integrals | render | P2 | pending | -- |
| 652 | TGV variational optical flow | tool | P2 | pending | -- |
| 653 | Mueller-matrix BRDF polarization decomposition | render | P2 | pending | -- |
| 654 | Optimal-transport photometric stereo inversion | tool | P2 | pending | -- |
| 655 | BRDF spectral eigenfunction decompositions | render | P2 | pending | -- |
| 656 | Calderon inverse scattering (DtN maps) | render | P2 | pending | -- |
| 657 | Harmonic light-field coordinate transforms | render | P2 | pending | -- |
| 658 | Airy/Pearcey coherent diffuse caustics | render | P2 | pending | -- |
| 659 | Lie-group spectral dispersion integration (no RGB hacks) | render | P2 | pending | -- |
| 660 | Spherical Radon transform ambient occlusion (no RTAO grain) | render | P1 | pending | -- |

## Cluster LXI: graphics hardware, silicon (661-670)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 661 | Silicon Plucker intersection units (no RT cores) | hw | OUT | out-of-scope | ADR-001 |
| 662 | Hilbert-curve hardware memory addressers | hw | OUT | out-of-scope | ADR-001 |
| 663 | Direct-silicon differential surface evaluators (no SPIR-V) | hw | OUT | out-of-scope | ADR-001 |
| 664 | Photonic rendering processing units | hw | OUT | out-of-scope | ADR-001 |
| 665 | Asynchronous independent-pixel core meshes (no SIMD) | hw | OUT | out-of-scope | ADR-001 |
| 666 | Hardware subdivision-matrix tessellation | hw | OUT | out-of-scope | ADR-001 |
| 667 | Clifford multivector ALUs (no matrix bloat) | hw | OUT | out-of-scope | ADR-001 |
| 668 | Non-volatile holographic framebuffers (no vsync) | hw | OUT | out-of-scope | ADR-001 |
| 669 | Silicon occlusion-culling crossbars (no early-Z) | hw | OUT | out-of-scope | ADR-001 |
| 670 | Self-reconfiguring mesh-shader arrays | hw | OUT | out-of-scope | ADR-001 |

## Cluster LXII: CFD, aerodynamics, microfluidics (671-680)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 671 | Vortex surface field hydrodynamics | fluid | OUT | out-of-scope | ADR-001 |
| 672 | Lattice Boltzmann D3Q27 MRT invariants | fluid | OUT | out-of-scope | ADR-001 |
| 673 | Conformal boundary-layer incompressibility | fluid | OUT | out-of-scope | ADR-001 |
| 674 | Symplectic smoke-jet advection (no BFECC) | fluid | OUT | out-of-scope | ADR-001 |
| 675 | Immersed boundaries on Cartesian meshes (no ALE) | fluid | OUT | out-of-scope | ADR-001 |
| 676 | Compressible Riemann solvers via wave trapping | fluid | OUT | out-of-scope | ADR-001 |
| 677 | Poisson-Boltzmann electrokinetic microfluidics | fluid | OUT | out-of-scope | ADR-001 |
| 678 | Vorticity-streamfunction inversion on manifolds | fluid | OUT | out-of-scope | ADR-001 |
| 679 | Co-volume Delaunay-Voronoi discretizations | fluid | OUT | out-of-scope | ADR-001 |
| 680 | Minimal-free-energy surface tension | fluid | OUT | out-of-scope | ADR-001 |

## Cluster LXIII: robotic kinematics, dynamics (681-690)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 681 | Screw-theory twist/wrench spatial algebra (no DH) | robot | OUT | out-of-scope | ADR-001 |
| 682 | Riemannian motion policies (no DMP tuning) | robot | OUT | out-of-scope | ADR-001 |
| 683 | Operational-space inertia-inverse control | robot | OUT | out-of-scope | ADR-001 |
| 684 | Lie-group kinodynamic planning on SE(3) (no RRT*) | robot | OUT | out-of-scope | ADR-001 |
| 685 | Port-Hamiltonian teleoperation networks | robot | OUT | out-of-scope | ADR-001 |
| 686 | Analytic kinematic SVD (no DLS guessing) | robot | OUT | out-of-scope | ADR-001 |
| 687 | Spang-Bullo underactuated reductions (no ZMP) | robot | OUT | out-of-scope | ADR-001 |
| 688 | Contact-implicit trajectory optimization | robot | OUT | out-of-scope | ADR-001 |
| 689 | Lie-algebraic inertial identification | robot | OUT | out-of-scope | ADR-001 |
| 690 | Cable-robot tension manifolds | robot | OUT | out-of-scope | ADR-001 |

## Cluster LXIV: geometric deep learning (691-700)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 691 | Gauge-equivariant mesh CNNs | ml | OUT | out-of-scope | ADR-001 |
| 692 | Steerable E(3) neural operators | ml | OUT | out-of-scope | ADR-001 |
| 693 | Riemannian manifold transformers | ml | OUT | out-of-scope | ADR-001 |
| 694 | Clifford group equivariant networks | ml | OUT | out-of-scope | ADR-001 |
| 695 | Laplace-Beltrami spectral wavelets | ml | OUT | out-of-scope | ADR-001 |
| 696 | Lie-group equivariant state filtering | ml | OUT | out-of-scope | ADR-001 |
| 697 | Sliced-Wasserstein point-cloud autoencoders | ml | OUT | out-of-scope | ADR-001 |
| 698 | Persistent-landscape differentiable embeddings | ml | OUT | out-of-scope | ADR-001 |
| 699 | Hyperbolic-curvature graph networks | ml | OUT | out-of-scope | ADR-001 |
| 700 | Sheaf neural diffusion networks | ml | OUT | out-of-scope | ADR-001 |

## Cluster LXV: acoustics, wave propagation, audio (701-710)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 701 | FM-BEM Helmholtz spatial audio (no HRTF tables) | audio | OUT | out-of-scope | ADR-001 |
| 702 | Biot-Allard poroelastic absorbers | audio | OUT | out-of-scope | ADR-001 |
| 703 | Lighthill-analogy turbulence sound synthesis | audio | OUT | out-of-scope | ADR-001 |
| 704 | Spectral-element nonlinear diffraction | audio | OUT | out-of-scope | ADR-001 |
| 705 | Eigenmode modal impact synthesis (no sample libs) | audio | OUT | out-of-scope | ADR-001 |
| 706 | Rayleigh-wave ground attenuators | audio | OUT | out-of-scope | ADR-001 |
| 707 | WKBJ atmospheric acoustic wavefields | audio | OUT | out-of-scope | ADR-001 |
| 708 | Fractional-derivative viscous damping | audio | OUT | out-of-scope | ADR-001 |
| 709 | Lagrangian sound-particle dissipation (no FDTD) | audio | OUT | out-of-scope | ADR-001 |
| 710 | Reciprocity-inversion microphone arrays | audio | OUT | out-of-scope | ADR-001 |

## Cluster LXVI: geomechanics, granular, terrain (711-720)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 711 | Drucker-Prager non-associated plasticity (no heightfields) | terrain | OUT | out-of-scope | ADR-001 |
| 712 | MPM multi-fluid phase transitions (no stitching) | terrain | OUT | out-of-scope | ADR-001 |
| 713 | Granular mu(I) rheology (no particle sand) | terrain | OUT | out-of-scope | ADR-001 |
| 714 | Terzaghi consolidation (no sinkhole heuristics) | terrain | OUT | out-of-scope | ADR-001 |
| 715 | Discrete fracture network geometries | terrain | OUT | out-of-scope | ADR-001 |
| 716 | Peridynamic non-local brittle fracture | terrain | OUT | out-of-scope | ADR-001 |
| 717 | Saint-Venant erosion-sedimentation solvers | terrain | OUT | out-of-scope | ADR-001 |
| 718 | Anisotropic thermal rock spalling | terrain | OUT | out-of-scope | ADR-001 |
| 719 | Mohr-Coulomb cap yield criteria | terrain | OUT | out-of-scope | ADR-001 |
| 720 | Richards-Darcy subsurface seepage | terrain | OUT | out-of-scope | ADR-001 |

## Cluster LXVII: optical systems, wavefronts (721-730)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 721 | Hamilton characteristics for aberrations (no lens raytrace) | render | P2 | pending | -- |
| 722 | Zernike wavefront expansion (no bokeh sprites) | render | P2 | pending | -- |
| 723 | ABCD Gaussian beam optics (no laser billboards) | render | P2 | pending | -- |
| 724 | Fresnel-Kirchhoff diffraction integrals (no bloom passes) | render | P2 | pending | -- |
| 725 | Kerr soliton light pipes (no threshold bloom) | render | P2 | pending | -- |
| 726 | DOE phase-shift profiles (no waveguide ghosts) | display | OUT | out-of-scope | ADR-001 |
| 727 | Birefringent splitting via Fresnel wave surfaces | render | P2 | pending | -- |
| 728 | Pancharatnam-Berry metasurfaces (no glass stacks) | display | OUT | out-of-scope | ADR-001 |
| 729 | Rayleigh-Sommerfeld angular-spectrum diffraction | render | P2 | pending | -- |
| 730 | Holographic SLM displays (no stereo 3D) | display | OUT | out-of-scope | ADR-001 |

## Cluster LXVIII: tissue, biomechanics (731-740)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 731 | Ogden hyperelastic tissue invariants | bio | OUT | out-of-scope | ADR-001 |
| 732 | HGO arterial fiber mechanics | bio | OUT | out-of-scope | ADR-001 |
| 733 | Fractional Kelvin-Voigt muscle models | bio | OUT | out-of-scope | ADR-001 |
| 734 | Biphasic cartilage mixture theory | bio | OUT | out-of-scope | ADR-001 |
| 735 | Huxley cross-bridge actuation PDEs | bio | OUT | out-of-scope | ADR-001 |
| 736 | Turing morphogenesis skin patterns (no painted textures) | bio | OUT | out-of-scope | ADR-001 |
| 737 | Wolff-law trabecular remodeling | bio | OUT | out-of-scope | ADR-001 |
| 738 | Epidermal buckling wrinkle generation | bio | OUT | out-of-scope | ADR-001 |
| 739 | Carreau-Yasuda blood hemodynamics | bio | OUT | out-of-scope | ADR-001 |
| 740 | Helfrich membrane mechanics | bio | OUT | out-of-scope | ADR-001 |

## Cluster LXIX: molecular dynamics (741-750)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 741 | Symplectic velocity-Verlet ensembles | mol | OUT | out-of-scope | ADR-001 |
| 742 | FMM electrostatic sums (no O(N^2)) | mol | OUT | out-of-scope | ADR-001 |
| 743 | EAM metallic bonding potentials | mol | OUT | out-of-scope | ADR-001 |
| 744 | MARTINI coarse-grained field dynamics | mol | OUT | out-of-scope | ADR-001 |
| 745 | Nose-Hoover Hamiltonian chains (no rescaling) | mol | OUT | out-of-scope | ADR-001 |
| 746 | Ewald k-space periodic summations | mol | OUT | out-of-scope | ADR-001 |
| 747 | Tersoff bond-order hydrocarbon potentials | mol | OUT | out-of-scope | ADR-001 |
| 748 | Overdamped Langevin colloidal diffusion | mol | OUT | out-of-scope | ADR-001 |
| 749 | Edwards field-theory polymer deformation | mol | OUT | out-of-scope | ADR-001 |
| 750 | Bond-boost hyperdynamics rare-event sampling | mol | OUT | out-of-scope | ADR-001 |

## Cluster LXX: non-Euclidean, relativistic graphics (751-760)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 751 | Hamiltonian geodesic ray tracing (no straight rays) | rel | OUT | out-of-scope | ADR-001 |
| 752 | Tetrad-frame relativistic transfer (no Doppler clipping) | rel | OUT | out-of-scope | ADR-001 |
| 753 | Hyperbolic Beltrami-Klein rasterization | rel | OUT | out-of-scope | ADR-001 |
| 754 | Kerr-Schild horizon penumbra integrators | rel | OUT | out-of-scope | ADR-001 |
| 755 | Wick-rotated Euclidean spacetime GI | rel | OUT | out-of-scope | ADR-001 |
| 756 | Penrose compactification mappings (no skybox clip) | rel | OUT | out-of-scope | ADR-001 |
| 757 | Synchrotron Stokes phase-space integrators | rel | OUT | out-of-scope | ADR-001 |
| 758 | Lorentz-boost viewport projections (no post warp) | rel | OUT | out-of-scope | ADR-001 |
| 759 | Cosmological redshift shift operators (no RGB tint) | rel | OUT | out-of-scope | ADR-001 |
| 760 | Yang-Mills volumetric field visualization | rel | OUT | out-of-scope | ADR-001 |

## Cluster LXXI: fabrication, topology optimization (761-770)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 761 | Homogenization multiscale elasticity tensors | mfg | P2 | pending | -- |
| 762 | Level-set topology optimization via shape derivatives | mfg | P2 | pending | -- |
| 763 | Geodesic-offset 5-axis toolpaths (no STL slicing) | mfg | OUT | out-of-scope | ADR-001 |
| 764 | Slicing-free curved deposition planning (no G-code) | mfg | OUT | out-of-scope | ADR-001 |
| 765 | SE(2) involute gear synthesizers (no undercut) | mfg | OUT | out-of-scope | ADR-001 |
| 766 | Stress-constrained topology slicing (no supports) | mfg | OUT | out-of-scope | ADR-001 |
| 767 | Medial-axis skeleton hollowing (no resin traps) | mfg | P2 | pending | -- |
| 768 | Anisotropic volumetric material distributions | mfg | P2 | pending | -- |
| 769 | Residual-stress inversion in sintering (no test prints) | mfg | OUT | out-of-scope | ADR-001 |
| 770 | Inertia-tensor rotor balance invariants | mfg | OUT | out-of-scope | ADR-001 |

## Cluster LXXII: vision, SLAM (771-780)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 771 | Essential-manifold logarithmic optimization (no 8-point) | slam | OUT | out-of-scope | ADR-001 |
| 772 | Gauge-free preconditioned bundle adjustment | slam | OUT | out-of-scope | ADR-001 |
| 773 | Conformal geometric algebra visual odometry | slam | OUT | out-of-scope | ADR-001 |
| 774 | Direct semi-dense photometric minimization (no ORB/SIFT) | slam | OUT | out-of-scope | ADR-001 |
| 775 | Spline-manifold spatio-temporal calibration | slam | P2 | pending | -- |
| 776 | Level-set variational surface tracking (no TSDF) | slam | OUT | out-of-scope | ADR-001 |
| 777 | Epipolar horizon clipping without rectification | slam | OUT | out-of-scope | ADR-001 |
| 778 | Persistent-homology loop closure (no BoVW) | slam | OUT | out-of-scope | ADR-001 |
| 779 | Sub-Riemannian wheel filters (no odometry drift) | slam | OUT | out-of-scope | ADR-001 |
| 780 | SE_2(3) invariant extended Kalman filtering | slam | OUT | out-of-scope | ADR-001 |

## Cluster LXXIII: spectral geometry, manifold invariants (781-790)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 781 | Heat-kernel signatures (no extrinsic descriptors) | mesh | P2 | pending | -- |
| 782 | Wave-kernel signatures (no heat over-smoothing) | mesh | P2 | pending | -- |
| 783 | Gromov-Wasserstein metric alignment (no landmarks) | mesh | P2 | pending | -- |
| 784 | Dirac-operator conformal mapping (no UV flips) | mesh | P2 | pending | -- |
| 785 | Cheeger-cut manifold partitioning (no manual segments) | mesh | P2 | pending | -- |
| 786 | Spectral-Hessian deformation modes (no cage rigs) | mesh | P2 | pending | -- |
| 787 | Dirichlet-to-Neumann shape signatures | mesh | P2 | pending | -- |
| 788 | Ollivier-Ricci curvature bottleneck analysis | mesh | P2 | pending | -- |
| 789 | Multi-parameter Betti persistence filtrations | mesh | P2 | pending | -- |
| 790 | Willmore-energy minimization surfaces (no C1 seams) | mesh | P2 | pending | -- |

## Cluster LXXIV: discrete differential geometry (791-800)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 791 | DEC divergence-free vector fields (no leakage) | mesh | P2 | pending | -- |
| 792 | Circle-pattern discrete conformal equivalence | mesh | P2 | pending | -- |
| 793 | Trivial-connection directional field design (no seams) | mesh | P2 | pending | -- |
| 794 | Unfolding-based parallel transport (no Christoffel) | mesh | P2 | pending | -- |
| 795 | Discrete Willmore flow on dual meshes (no shrinkage) | mesh | P2 | pending | -- |
| 796 | Hodge-Helmholtz boundary projectors (no mass leak) | mesh | P2 | pending | -- |
| 797 | Discrete quaternionic spinor immersions | mesh | P2 | pending | -- |
| 798 | Intrinsic Delaunay refinement (no remeshing) | mesh | P2 | pending | -- |
| 799 | DEC Maxwell integrators (no Yee staircasing) | mesh | P2 | pending | -- |
| 800 | Curvature-preserving polyhedral decimation (LOD generation) | mesh | P0 | adopted | ADR-006 |

## Cluster LXXV: non-Newtonian, viscoplastic materials (801-810)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 801 | Bingham-Papanastasiou yield-stress integrators | fluid | OUT | out-of-scope | ADR-001 |
| 802 | Herschel-Bulkley power-law shear solvers | fluid | OUT | out-of-scope | ADR-001 |
| 803 | Thixotropic structural-breakdown automata | fluid | OUT | out-of-scope | ADR-001 |
| 804 | Marangoni solutocapillary dynamics | fluid | OUT | out-of-scope | ADR-001 |
| 805 | Phase-inversion emulsion coalescence | fluid | OUT | out-of-scope | ADR-001 |
| 806 | Giesekus viscoelastic tensor models | fluid | OUT | out-of-scope | ADR-001 |
| 807 | Capillary-bridge granular cohesion | fluid | OUT | out-of-scope | ADR-001 |
| 808 | Casson blood-rheology solvers | fluid | OUT | out-of-scope | ADR-001 |
| 809 | Phillips suspension-balance migration | fluid | OUT | out-of-scope | ADR-001 |
| 810 | Larche-Cahn stress-coupled diffusion | fluid | OUT | out-of-scope | ADR-001 |

## Cluster LXXVI: metamaterials, micro-architected solids (811-820)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 811 | Homological auxetic generators (nu < 0) | mfg | OUT | out-of-scope | ADR-001 |
| 812 | Phononic bandgap lattice synthesizers | mfg | OUT | out-of-scope | ADR-001 |
| 813 | Curvature-induced bistable shells | mfg | OUT | out-of-scope | ADR-001 |
| 814 | Kirigami Ricci-flow tessellations | mfg | OUT | out-of-scope | ADR-001 |
| 815 | Spinodal Cahn-Hilliard microstructures | mfg | OUT | out-of-scope | ADR-001 |
| 816 | Thermal-expansion inversion metamaterials | mfg | OUT | out-of-scope | ADR-001 |
| 817 | Origami degree-4 foldability solvers | mfg | OUT | out-of-scope | ADR-001 |
| 818 | Graded-TPMS gyroid modulators | mfg | OUT | out-of-scope | ADR-001 |
| 819 | Inverse-stress load-path synthesis | mfg | OUT | out-of-scope | ADR-001 |
| 820 | Elastic soliton pipelines | mfg | OUT | out-of-scope | ADR-001 |

## Cluster LXXVII: audio DSP, spectral synthesis (821-830)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 821 | Wave-digital-filter circuit modeling (no SPICE lag) | audio | OUT | out-of-scope | ADR-001 |
| 822 | Functional-transformation string synthesis (no samples) | audio | OUT | out-of-scope | ADR-001 |
| 823 | Volterra-series saturation DSP (no aliasing) | audio | OUT | out-of-scope | ADR-001 |
| 824 | Stiefel-manifold phase-vocoder resynthesis | audio | OUT | out-of-scope | ADR-001 |
| 825 | Unitary-scattering feedback delay networks | audio | OUT | out-of-scope | ADR-001 |
| 826 | Pre-warped bilinear filter transforms (no cramping) | audio | OUT | out-of-scope | ADR-001 |
| 827 | ERB auditory-masking equalization | audio | OUT | out-of-scope | ADR-001 |
| 828 | SVD-Hankel modal noise reducers | audio | OUT | out-of-scope | ADR-001 |
| 829 | Kelly-Lochbaum vocal-tract synthesis | audio | OUT | out-of-scope | ADR-001 |
| 830 | NLMS acoustic echo cancellation | audio | OUT | out-of-scope | ADR-001 |

## Cluster LXXVIII: computational geometry, booleans (831-840)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 831 | Exact multi-precision geometric predicates (no float crashes) | asset | P0 | adopted | ADR-003 |
| 832 | CAD surface arrangements via cylindrical decomposition | cad | P2 | pending | -- |
| 833 | Sweep-line curved-envelope traversal (no O(N^2)) | mesh | P2 | pending | -- |
| 834 | Avis-Fukuda reverse-search convex hulls (no QuickHull crash) | phys | P2 | pending | -- |
| 835 | Nef-polyhedra B-Rep booleans (no non-manifold failure) | cad | P1 | pending | -- |
| 836 | SDF zero-crossing mesh slicing (no STL repair) | tool | P2 | pending | -- |
| 837 | Seidel trapezoidal polygon triangulation (no ear-clip O(N^2)) | mesh | P2 | pending | -- |
| 838 | Continuous alpha-shape molecular envelopes | mesh | P2 | pending | -- |
| 839 | Matrix-pencil contact distance for moving ellipsoids | phys | P2 | pending | -- |
| 840 | Kinetic data structures for Delaunay tracking (no rebuilds) | phys | P2 | pending | -- |

## Cluster LXXIX: computational photography (841-850)

| ID | Paradigm | Area | Tier | Status | ADR |
|----|----------|------|------|--------|-----|
| 841 | Non-local-means graph-Laplacian denoising (no halos) | tool | P2 | pending | -- |
| 842 | Rudin-Osher-Fatemi primal-dual deblurring (no ringing) | tool | P2 | pending | -- |
| 843 | Color-difference spectral demosaicing (no moire) | sensor | OUT | out-of-scope | ADR-001 |
| 844 | Compressive hyperspectral unmixing (no metamerism) | sensor | OUT | out-of-scope | ADR-001 |
| 845 | Poisson gradient-domain fusion (no seams) | tool | P2 | pending | -- |
| 846 | Retinex illumination-reflectance decomposition | tool | P2 | pending | -- |
| 847 | Fourier-slice plenoptic refocusing (no autofocus lag) | sensor | OUT | out-of-scope | ADR-001 |
| 848 | Watershed topological-flooding segmentation | tool | P2 | pending | -- |
| 849 | Bilateral base/detail tone mapping (no halos) | render | P1 | pending | -- |
| 850 | Radial-polynomial chromatic-aberration deconvolution | render | P2 | pending | -- |

## Scoreboard

- Pending P0: none -- queue drained. Decided: 554 rejected (ADR-002), 831 adopted (ADR-003), 608 adopted live (ADR-004), 611 adopted as utility (ADR-005), 800 adopted as utility (ADR-006).
- Pending P1: 558, 566, 587, 590, 591, 594, 595, 602, 606, 619, 622, 623,
  626, 630, 632, 633, 635, 638, 660, 835, 849.
- Pending P2: everything else not OUT.
- Out of scope (ADR-001): 561, 567, 571, 610, all of LIX, LXI, LXII, LXIII,
  LXIV, LXV, LXVI, LXVIII, LXIX, LXX, LXXV, LXXVI, LXXVII, 726, 728, 730,
  763-766, 769, 770, 771-774, 776-780, 843, 844, 847.
- Adopted: 831 (utility), 608 (live), 611 (utility), 800 (utility). Rejected: 554.
