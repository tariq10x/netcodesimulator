# NetcodeSim Architecture Overview

## Purpose

NetcodeSim is a local-first educational FPS networking sandbox. Its purpose is not to be a production online shooter. Its purpose is to let people see, manipulate, and reason about the systems that sit underneath multiplayer FPS games:

- latency and jitter
- packet loss and transport artifacts
- tick-driven simulation
- client prediction
- interpolation
- lag compensation
- authoritative hit validation
- snapshot replication
- the split between what the player sees and what the server accepts as truth

The project is meant to work as both a learning tool and a simulation tool. Users should be able to play with the runtime, change network conditions, and build intuition for why multiplayer FPS games feel the way they do. In its current form, it is a local/LAN study platform rather than an internet-scale live service game.

## What NetcodeSim Currently Implements

At a high level, the current codebase already implements a meaningful end-to-end multiplayer FPS study stack:

- a menu-driven application shell
- a local host/join multiplayer flow
- LAN session discovery
- a UDP-based client/server runtime
- a transport-artifact proxy that can inject delay, loss, jitter, duplication, and reordering
- authoritative server simulation
- client-side prediction
- remote entity interpolation
- server-side lag compensation
- configurable shot evaluation rules
- proxy-backed diagnostics and local network parameter controls
- team selection, score tracking, respawns, and simple bots
- a study-oriented sandbox flow
- a level editor and slot-based level loading

This means NetcodeSim is already more than a rendering demo. It has a real gameplay/runtime architecture centered on the shared client/server session stack.

The active user-facing app surface is built around `Multiplayer`, `Lab Study`, `Level Editor`, `Replay Studio`, and `Settings`.

## What The Simulator Is For

The simulator is designed to teach the structure of a multiplayer FPS from the inside out.

From the player side, it helps explain questions like:

- Why does another player look delayed?
- Why does movement become choppy at lower tick rates?
- Why does interpolation smooth motion without removing latency?
- Why can a shot feel correct on the attacker side but look wrong on the defender side?
- Why does lag compensation exist?

From the architecture side, it helps explain how multiplayer FPS games are typically organized:

- the client captures input and builds commands
- the server owns the authoritative simulation
- the network path introduces delay and loss
- the client reconstructs a playable view from delayed snapshots
- hit registration depends on both transport timing and server policy

The project therefore has two equally important roles:

1. A visual learning environment for networking concepts.
2. A small, understandable reference architecture for multiplayer FPS structure.

## Current Architectural Shape

The current implementation is organized around a shared session stack that follows a client/server/protocol model. The app shell starts typed flows, the shared runtime composes hosting, proxy, and client pieces, and the simulation layer keeps deterministic gameplay logic separate from rendering and transport concerns.

## High-Level Runtime Layers

### 1. Application Shell

The top-level app owns the overall user flow:

- main menu and navigation
- level selection
- host/join setup
- session startup and shutdown
- switching between study, multiplayer, and editing flows

This layer acts as the composition root. It decides which runtime should be assembled, but it is not meant to own the detailed simulation rules.

### 2. Client Runtime

The client side is responsible for the player-facing runtime:

- reading input
- managing connection state
- building and sending player commands
- receiving world snapshots
- applying prediction and interpolation
- building HUD, diagnostics, and presentation state
- exposing the current session-facing study controls and status

This is the side that represents what the player experiences, including the gap between immediate local responsiveness and delayed authoritative truth.

### 3. Server Runtime

The server side owns the authoritative game state:

- player acceptance and session membership
- authoritative world ticking
- combat and damage resolution
- lag compensation
- respawns and score updates
- bot participation
- snapshot generation for clients

This is the side that decides what is actually true in the simulation.

### 4. Transport and Networking Layer

The networking layer is not just raw sockets. It also contains the study machinery that makes the simulator useful:

- UDP transport
- packet protocol and serialization
- LAN discovery messages
- host/join launch configuration
- a proxy runtime that simulates transport artifacts
- diagnostics and runtime parameter control surfaces

This layer is where NetcodeSim becomes a learning tool rather than only a game prototype.

### 5. Shared Simulation and Contract Layer

A shared layer already exists in practice through the protocol and simulation types:

- shared packet contracts
- snapshot and event structures
- world state and player state
- simulation rules
- timing/tick data
- level identity and session metadata

This layer is the common language between client and server. It is what makes the architecture understandable and teachable.

### 6. Rendering and Gameplay Presentation Shells

The project still contains large presentation-oriented mode shells, especially around the older study/gameplay flows. These provide:

- 3D scene rendering
- camera and spectator behavior
- overlays and explanatory visuals
- compatibility study flows that still bridge older and newer runtime ideas

These shells are important today because they hold much of the educational user experience, even though they are not the long-term architectural ideal.

## User-Facing Product Scope Today

Today, NetcodeSim is already scoped as a small multiplayer FPS study platform with three main kinds of activity:

- Play or study a networked FPS session.
- Manipulate transport conditions and observe the effect.
- Build repeatable scenarios through authored levels and controlled local session setup.

In practical terms, the implemented product scope includes:

- hosting a local session
- joining a hosted session
- browsing LAN sessions
- running Lab Study as a local hosted session against bots
- toggling and studying prediction and interpolation behavior during active sessions
- selecting the shot evaluation rule during host setup
- changing proxy-backed latency/loss-style parameters at runtime in proxied sessions
- observing diagnostics, score, and combat feedback
- creating and loading simple level layouts

Legacy note:

- spectator, split-screen, and old replay/checkpoint interactions are still present in compatibility-oriented code paths
- those features are not currently part of the main shared session flow documented for end users

## Current Architectural Strengths

The current implementation already has the right core ideas for an educational multiplayer FPS:

- There is a real separation between client responsibility and server authority.
- There is a real transport boundary instead of only fake visual delay.
- There are explicit protocol contracts and snapshots.
- The simulator exposes the exact systems people need to learn from: prediction, interpolation, lag compensation, and transport artifacts.
- The app already supports both structured multiplayer flow and controlled study flow.

Those are the foundations needed for NetcodeSim to become a credible learning reference.

## Current Cleanup Direction

The old mode-specific gameplay shells and fake-network study path have been removed from the primary codebase. Remaining cleanup work should stay focused on tightening the current shared boundaries:

- keep session startup in the app/session-composition layer
- keep deterministic gameplay in the simulation and server layers
- keep presentation state flowing through typed client view models
- keep educational study tools attached to the shared runtime instead of reintroducing mode-specific runtime loops

## What This Means For The Goal Architecture

The architectural direction should stay grounded in the current strengths of the project:

- keep NetcodeSim as a hands-on learning tool first
- keep the transport boundary visible and configurable
- keep the authoritative client/server model central
- keep the code understandable enough that people can learn how FPS networking is structured

The goal is not just to make the code cleaner. The goal is to make the architecture itself part of the teaching experience.

In other words, NetcodeSim should help users understand both:

- what happens inside a multiplayer FPS at runtime
- how those responsibilities are typically divided in a well-structured game architecture

## Summary

NetcodeSim currently implements a real educational multiplayer FPS runtime, not just a visualization prototype. It already has the essential pieces needed to teach modern FPS networking:

- authoritative server simulation
- client responsiveness systems
- transport artifact simulation
- runtime diagnostics
- shared-session study tooling
- host/join session flow

Its current architecture is now centered on the shared runtime stack, with remaining work focused on keeping that stack compact and easy to understand as a reference project for multiplayer FPS networking.
