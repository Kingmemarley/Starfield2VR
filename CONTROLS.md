# Starfield2VR controls

This guide uses Meta Touch (Quest) names:

- **Left controller:** X (lower button), Y (upper button), the menu button (three lines).
- **Right controller:** A (lower button), B (upper button).
- **Both:** trigger (index finger), grip (middle finger), thumbstick (it also clicks), and **thumbrest**: the flat spot next
  to the face buttons where your thumb rests. The mod can tell when your thumb touches it.

Everything here is the **default layout**. The [Alternative Joy Layout](#alternative-joy-layout),
[left-handed play](#left-handed-play-swap-controller-inputs) and [other controllers](#other-controllers) are at the end.
Parts that have not been tested yet say so.

## Contents

- [How the mod talks to the game](#how-the-mod-talks-to-the-game)
- [Moving and looking](#moving-and-looking)
- [Guns](#guns)
- [Melee](#melee)
- [Dual wielding (a weapon in your left hand)](#dual-wielding-a-weapon-in-your-left-hand)
- [Hands and grabbing](#hands-and-grabbing)
- [People](#people)
- [Powers](#powers)
- [Grenades](#grenades)
- [Hand scanner and flashlight](#hand-scanner-and-flashlight)
- [Climbing and ladders](#climbing-and-ladders)
- [Menus](#menus)
- [Conversations](#conversations)
- [Favorites wheel](#favorites-wheel)
- [Star map](#star-map)
- [Build mode](#build-mode)
- [The pilot's seat](#the-pilots-seat)
- [Eye modes, flat screen and the mod's menu](#eye-modes-flat-screen-and-the-mods-menu)
- [Alternative Joy Layout](#alternative-joy-layout)
- [Left-handed play (Swap Controller Inputs)](#left-handed-play-swap-controller-inputs)
- [Other controllers](#other-controllers)
- [Settings that change the controls](#settings-that-change-the-controls)

---

## How the mod talks to the game

The mod plugs in a virtual Xbox controller (this is why ViGEmBus is needed) and turns your VR controllers into its buttons.
So the game's own controller bindings still apply. The right-hand column shows Starfield's **default** gamepad action. If
you changed the game's bindings, that column changes too.

| VR input | Gamepad button it sends | Starfield's default action |
| - | - | - |
| Right **A** | A | Use / take / talk. Hold on an object: the game's own grab |
| Right **B** | Y | Jump. In the air: boost pack |
| Left **X** | B | Sneak. In menus: back |
| Left **Y** | X | Reload. Hold: put your weapon away or draw it |
| Left thumbstick | Left stick | Move |
| Left thumbstick click | Left stick click | Sprint |
| Right thumbstick | Right stick | Turn (see [Moving and looking](#moving-and-looking)) |
| Right thumbstick click | Right stick click | Melee |
| Right trigger | RT | Fire |
| Right grip | LT | Aim |
| Left thumbrest, touched | LB, a short tap | Hand scanner on or off |
| Right thumbrest, thumb resting | RB, held while your thumb rests there | Grenade (thrown as your thumb lifts) |
| Left menu button | Start (Menu) | Data menu |
| Left grip held + left menu button | Back (View) | Switch first / third person |
| Palm push (see [Powers](#powers)) | LB + RB together | Your power |
| D-pad | Only from combos (see [Favorites wheel](#favorites-wheel)) | Quick keys |

- The left trigger sends nothing to the game in play. It has its own jobs (the left-hand weapon, the favorites wheel, the
  flashlight).
- The thumbrests never send LB and RB at the same time, because together they are your power. If one is already down, the
  other waits until it is let go.
- A stick click is sent once it is clear the other stick is not clicking too, after 0.1 s. A quicker click is sent as a short
  press when you let go. Clicking both sticks opens the mod's menu and sends nothing to the game.
- If you press no controller button for 30 s, the controllers count as idle and send nothing. Press any button to wake them.

---

## Moving and looking

| Input | What it does |
| - | - |
| Left thumbstick | Walk |
| Left thumbstick click | Sprint |
| Right thumbstick left / right | Snap turn, 45 degrees (default), or smooth turn. Push past about 70%, and let the stick come back before the next snap |
| Right thumbstick up / down | Nothing on foot while "Level view" is on (default): you look up and down with your head |
| Right **B** | Jump. Press again in the air for the boost pack |
| Left **X** | Sneak (crouch). The view drops to your crouched body's eyes ("Crouch button lowers your view") |
| Left grip + left menu button | Switch first / third person (the game's View button) |

- The turn mode, snap angle and smooth speed are in the game's Settings > **VR** tab, and in F11.
- In menus, in the pilot's seat and in third person, the right stick is the game's own.
- Your body turns with your head ("Pawn Control Rotation", on by default).

---

## Guns

| Input | What it does |
| - | - |
| Right trigger | Fire. With your weapon put away, the game draws it |
| Right grip | Aim (the game's LT) |
| Left hand near the gun | Two-handed grip. It happens by itself, no button needed |
| Left **Y**, tap | Reload |
| Left **Y**, hold | The game puts your weapons away, or draws them (both hands' weapons) |

- With the weapon out, shots follow your right hand and leave from the gun's muzzle.
- The red dot or laser shows where shots go.
  - With DLSS, the dot lands on what you point at.
  - Without DLSS, the dot sits at a fixed distance (15 m by default).
  - Turn the game's own crosshair off in Settings > Interface.

---

## Melee

| Input | What it does |
| - | - |
| Swing hard (fist, blade or gun, weapon out) | Hits what your hand or weapon swept through. With "instant hits" (default) every swing counts, with no attack animation and no oxygen cost |
| Right thumbstick click | The game's own melee: a jab where your right hand is |
| Right trigger, melee weapon in your right hand | Nothing: you swing to strike instead. With the weapon put away the trigger still draws it |
| Right grip, melee weapon out | Sends the game's LT, which blocks with a melee weapon (not tested yet). It also gets the weapon ready to throw |
| Your blade in the path of an enemy's melee hit | The hit misses and the attacker recoils (the hand buzzes) |

**Throwing your melee weapon** (a lightsaber, sword, axe...):

| Input | What it does |
| - | - |
| Squeeze the grip, swing, and let go of the grip during the swing | Throw it. The swing must be fast (at least 2.5 m/s) |
| Keep the grip squeezed before throwing | Locks onto the person nearest the line your hand points along (a buzz). The throw flies at them |
| Hold the trigger after the throw | Guide it: it turns toward where your hand points |
| Let go of the trigger | It comes back |
| Squeeze the grip again | It comes back at once |
| Do nothing | It flies on, and comes back after 20 m or 2 s. It is caught by itself when it reaches your hand |

- It hits each person on the way out and on the way back, and it bounces back off walls and the ground.
- It stays equipped: it never leaves your inventory.
- While it is away, your empty hand does not strike and the right trigger does nothing in the game.

---

## Dual wielding (a weapon in your left hand)

The game has one weapon slot. Your left-hand weapon is the mod's own copy of a melee weapon you carry.

| Input | What it does |
| - | - |
| In the **inventory**: point your **left** hand at a melee weapon, pull the **left** trigger | It goes to your left hand. Do it again on the same weapon to take it out. The left trigger on anything else does nothing |
| **Favorites wheel**: left hand on a melee weapon's slice, let go of the left trigger | It goes to your left hand |
| The game's own **favorites menu** (when the wheel is off): left trigger on a melee weapon | It goes to your left hand |
| Left trigger, in play | Draws your left-hand weapon when it is put away |
| Left **Y** twice quickly (within 0.4 s) | Puts the left weapon away, or draws it again. Each press is still a reload tap for the game |
| Left **Y** held 0.35 s while only the left weapon is out | Puts the left weapon away on its own |
| Swing your left hand | Strike with it |
| Left grip, swing, let go | Throw it. The left trigger guides it, and the left grip calls it back |

- To hold the same weapon in both hands you must carry two of it.
- The left weapon hides while you sit, in menus, in conversations, in the pilot's seat and on ladders.

---

## Hands and grabbing

These work with your **weapon put away, or with only your fists out**.

| Input | What it does |
| - | - |
| Squeeze a grip next to a loose object | Grab it. It sits in your hand and turns with it |
| Let go of the grip | Drop it |
| Let go while swinging | Throw it, at your hand's speed |
| Other hand: squeeze its grip on the held object | Hold it with both hands |
| Other hand: squeeze its grip away from the held object | Grab something else: one object in each hand |
| Squeeze a grip with nothing near your hand | **Gravity gloves** aim. What they will pull gets a blue mark, and turns white once locked on (with a buzz) |
| ...then flick your hand back toward you | It flies to your hand. Keep the grip squeezed, or squeeze it again within about a second, to catch it |
| Hold food or a drink at your mouth (a steady buzz), let go | Eat or drink it |
| Let go of an item behind a shoulder or at a hip (a light buzz) | It goes in your inventory |
| Hold a weapon you found, pull its trigger | Take it and equip it. Drop it, or put it in your bag, to get your old weapon back |
| Right hand near something, press right **A** | Use it. With your weapon put away, the "Take / Open / Talk" prompt is for whatever is near your hand (within 35 cm) |
| Hold right **A** on an object | The game's own grab. Squeezing a grip then takes it into that hand |

- Something too heavy for the gloves gives a long soft buzz, and the gloves skip it after that.
- The gloves reach 5 m by default. "Gloves point with" lets you aim with your hand (default), your palm, or by sighting
  along your hand.
- With any weapon out other than fists, the grips do not grab.

---

## People

With your **weapons put away** (punches: fists out).

| Input | What it does |
| - | - |
| Squeeze a grip on a standing person | Hold them standing, their arm bent toward your hand. Walk to drag them |
| Squeeze a grip on a body, or on someone knocked down | Drag them by the limb you touched. They are let go if stretched too far (0.7 m) |
| Push an open hand into someone | Bump them (light push) or make them stagger (firm push) |
| Fists out, punch hard (6 m/s by default) | Knock them down. The game gets them back up |

- Shoving or knocking down the same people again and again makes them angry (the game's assault alarm).
- Followers, children, and people sitting or talking are never staggered or knocked down. Your hand's hits never hurt your
  followers.

---

## Powers

| Input | What it does |
| - | - |
| With an empty hand (not the gun hand): squeeze its grip, turn your palm outward, and push the hand briskly toward the target | Uses your equipped power (the game's LB + RB), aimed along your push |

To count as a push:
- squeeze the grip as the push starts, at most 0.25 s after it begins;
- move the hand at least 22 cm within 0.35 s, at 1.5 m/s or more (setting "Push speed");
- keep the palm facing within 40 degrees of the push, and start at least 30 cm from your eyes;
- wait 1 s between powers.

There are no powers in the hand scanner, in build mode, while climbing, or for 1 s after the favorites wheel closes. Choose
your power on the favorites wheel or in the game's Powers menu.

---

## Grenades

Equip a grenade first (it has its own slot).

| Input | What it does |
| - | - |
| Reach your right hand slowly behind your right shoulder and squeeze the right grip | Take a grenade (a buzz) |
| Bring the hand forward | The game readies it (RB held) |
| Let go of the grip as you throw | Thrown along your hand's throw. A throw slower than 1 m/s uses the game's own direction |
| Let go before your hand leaves the shoulder | Put it back |
| Rest your right thumb on the right thumbrest, then lift it | The game's own grenade throw (RB held while your thumb rests) |

A copy of the grenade shows in your hand while you hold it. It cannot be picked up.

---

## Hand scanner and flashlight

| Input | What it does |
| - | - |
| Touch the left thumbrest | Hand scanner on or off (LB tap). It does nothing while the left grip is squeezed |
| Right hand beside your right temple, pull the right trigger | Hand scanner on or off |
| Left hand beside your left temple, pull the left trigger | Flashlight on or off. No helmet needed |

**In the hand scanner:**

| Input | What it does |
| - | - |
| Left grip | LB: closes the scanner |
| Right grip | RB: the surface map |
| Right thumbrest | Nothing (its RB would open the surface map by accident) |

The trigger used at a temple does not also fire or aim. It is kept from the game until you let go.

---

## Climbing and ladders

**Climbing walls, rocks and ledges** (weapon put away or fists only):

| Input | What it does |
| - | - |
| Put a hand against a wall, rock face or ledge, squeeze its grip | Hold on (a short buzz) |
| Pull the hand | Move yourself the other way, so the hand stays where it holds. With both hands holding, their pulls are averaged |
| Let go of every grip | Drop: a normal fall, with the game's fall damage from where you let go |

- While you hold on, the right trigger, right **A** and the left stick are kept from the game. Jump (right **B**) is not, so
  leave it alone while you hold on.
- If an object or a person is next to your hand, the grip grabs that instead.
- "Climbing pull" makes each pull move you further (useful seated).

**Ladders** (you stay in first person, seeing from your body's eyes):

| Input | What it does |
| - | - |
| Stand at a ladder's end, press right **A** | Get on |
| Squeeze a grip and pull down | Climb up |
| Squeeze a grip and push up | Climb down |
| Hold the hand still | Stop |
| Left thumbstick up / down | Climb as well |
| Right thumbstick | Turn your view |

The game takes you off at the top or the bottom.

---

## Menus

Menus show on a panel that stays where it was put. Recenter (the mod's or your headset's) to bring it in front of you.

| Input | What it does |
| - | - |
| Point either hand at the panel | A beam and a dot. If both hands point, the hand that clicked last wins |
| Pointing hand's trigger | Click |
| Right thumbstick up / down while pointing | Scroll |
| Left thumbstick | Move through the menu as on a gamepad. The pointer steps aside until you move your hand or pull a trigger. A trigger then picks the highlighted item (as A) |
| Left thumbstick left / right, in menus with side tabs (missions, crew, skills, research, almanac, ship; also trading and containers) | The menu's left / right step |
| Left / right thumbrest (while not pointing) | LB / RB: the menu's previous / next tab |
| Right **A** | Select |
| Left **X** | Back / close |
| Left **Y**, right **B** | The menu's own X and Y actions (see the hints at the bottom). In Settings, Y is Defaults |
| Left menu button | Start: the Data menu. From the Data menu, the pause menu |
| Left grip + left trigger + left thumbstick | D-pad |

- While you point, the sticks, triggers and thumbrests are kept from the game, so a resting thumb does not switch tabs.
- In the game's favorites menu the left thumbstick alone moves through the slots. A fresh trigger pull picks the selected
  favorite: the right trigger for the right hand, the left trigger for a melee weapon in the left hand.
- **The VR tab:** Pause > Settings > VR has turning, vibration and climbing settings.

---

## Conversations

The conversation shows on its own panel in front of the person speaking (60 cm from your eyes by default).

| Input | What it does |
| - | - |
| Point a hand at an answer, pull that hand's trigger | Choose it |
| Right thumbstick up / down while pointing | Scroll the answers |
| Right thumbstick left / right | Turn smoothly, whatever your turn mode |
| Push your index finger into an answer | Choose it. Off by default (setting "press an answer by pushing your index finger into it") |
| Right **A** | The game's own choose |

---

## Favorites wheel

A wheel at your left hand, in place of the game's favorites menu (on by default).

| Input | What it does |
| - | - |
| Hold the **left grip**, then squeeze the **left trigger** (left stick in the middle) | Opens the wheel at your left hand. Time slows, as in the game's own favorites |
| Move a hand over a slice | That hand picks it: a buzz, and its name shows in the middle. The middle circle picks nothing |
| Right trigger on a slice | Use it now. The wheel stays open (several medpacks in a row) |
| Let go of the **left trigger** | Choose and close (see below) |

What letting go of the left trigger does:
- the right hand's slice goes to the right hand: a weapon is drawn, an item used, or a power chosen;
- a melee weapon under the left hand goes to the left hand. Anything else under it is used as usual;
- with no slice picked, the wheel closes and nothing is chosen.

- Only the left hand's wheel is shown. Your right hand picks on an unseen copy, centred where your right hand was when the
  wheel opened. Its pick lights up on the wheel you see.
- While the wheel is open, no trigger, grip, shoulder button or D-pad reaches the game.
- The wheel does not open:
  - in menus, build mode, conversations, the pilot's seat, or on loading screens;
  - while the F11 menu is open;
  - while your left hand holds something, has its weapon thrown, or has the gravity gloves locked on something.
- It is off with the Alternative Joy Layout, with Swap Controller Inputs, and without OpenXR.
  - The game's own favorites menu then works as before.
  - Its D-pad is left grip + left trigger + left stick (in the alternative layout: left grip + left stick).
- While the wheel can be used, the D-pad combo sends nothing in play.

---

## Star map

The star map (and a planet's view) is a menu: point and click with the triggers as above.

| Input | What it does |
| - | - |
| Left grip | LB (a planet's view: its resources) |
| Right grip | RB (a planet's view: its surface map) |
| Right thumbstick up / down | Zoom in / out, whether you point or not |
| Right thumbstick left / right | The game's own turn |
| Left grip + left trigger + left thumbstick | D-pad (moves around the map) |
| Left grip + left menu button | View (missions) |

The left grip's LB waits 0.15 s, so that a combo (trigger, menu button, stick click) can join it first.

---

## Build mode

Ship decorating and outpost building. These are the buttons the mod sends for the game's own build-mode controls. Not
all of them have been tried in VR yet. Your hands' own features (grab, gloves, powers, grenades, temples, swing melee,
the left weapon, the wheel) are off here.

| Input | What it does |
| - | - |
| Left grip / right grip | LB / RB: change category |
| Left trigger (without the left grip) | LT: rotate left |
| Right trigger | RT: rotate right |
| Left grip + left trigger + left thumbstick | D-pad: move through the list, choose variants |
| Left grip + left menu button | View |
| Left thumbstick | Walk |
| Right **A** | Place / build (the game's A) |
| Left **Y** | Delete (the game's X) |
| Right thumbstick click | Precise placement (the game's right stick click) |

The build menu shows as panels low in front of you ("Build mode as a console").

---

## The pilot's seat

The buttons go to the game as in [the table at the top](#how-the-mod-talks-to-the-game). Use Starfield's own ship
controls. The right thumbstick is the game's own here (no snap turning). The favorites wheel, the left-hand weapon and the
hand gestures are not used in the seat.

---

## Eye modes, flat screen and the mod's menu

| Input | What it does |
| - | - |
| Left grip held + right thumbstick click | Next eye mode: stereo > mono > AER > stereo. A short buzz means mono, a long buzz AER, a medium buzz stereo. Two short buzzes: refused, because video memory is full |
| Left grip held + left thumbstick click | Flat screen on or off: no VR camera, the game on a flat virtual screen. Only when the left grip is not holding, aiming or climbing |
| **F12** (with the game's window in front) | Flat screen on or off. F12 is also Steam's screenshot key |
| Both thumbstick clicks together | The mod's menu on or off |
| **F11** | The mod's menu on or off. Use the mouse in it |

In F11 > VR you can also bind keyboard keys to: Recenter View, Set Standing Origin, and the Mono / AER toggle.

---

## Alternative Joy Layout

F11 > CreationEngine > "Alternative Joy Layout" (off by default). It is for controllers without thumbrest sensors, or for
shoulder buttons on the triggers. Not tested yet.

| Input | Default layout | Alternative layout |
| - | - | - |
| Thumbrests | LB tap (left), RB held (right) | Nothing |
| Left grip + right trigger | Fire | RB (no fire) |
| Left grip + left trigger | Favorites wheel | LB |
| Left grip + both triggers | Favorites wheel | LB + RB: your power |
| D-pad | Left grip + left trigger + left thumbstick | Left grip + left thumbstick. Not while a left-hand weapon is held: that grip throws it |
| Favorites wheel | On | Off: the game's own favorites menu |
| Grips as LB / RB in the scanner, star map and build mode | On | Off |

---

## Left-handed play (Swap Controller Inputs)

F11 > VR > "Swap Controller Inputs (left-handed)" (off by default). Not tested yet.

The two controllers swap jobs:
- the left controller fires, aims, turns and does what A and B do, and your left shoulder is the grenade shoulder;
- the right controller walks and does what X and Y do.

What the code shows:
- The thumbrests stay on their own side: the left thumbrest is still LB, the right one RB.
- The D-pad combo still reads the physical left thumbstick.
- The favorites wheel is off (the game's own favorites menu instead).
- In menus, each hand's own trigger clicks for that hand.

---

## Other controllers

The mod has OpenXR bindings for Meta Touch, Valve Index, HTC Vive, Windows Mixed Reality and the basic OpenXR controller.
Only Meta Touch has been tested.

- **Index:** each hand's A and B work as on Touch (the left A as X, the left B as Y). There is no thumbrest, so use the
  Alternative Joy Layout.
- **Vive wands and Windows Mixed Reality:** clicking the touchpad near an edge stands in for the face buttons:
  - left pad: bottom = X, right = Y, top = stick click;
  - right pad: bottom = A, left = B, top = stick click.
- F11 > VR > Bindings can change the OpenXR bindings.

---

## Settings that change the controls

All of these are in the F11 menu. The turning settings are also in Settings > VR.

| Setting | Default | What it changes |
| - | - | - |
| Turning (right stick) | Snap | Snap or smooth |
| Snap angle | 45 degrees | 15 to 90 degrees, in steps of 5 |
| Smooth turn speed | 1.0 | 1 is the game's own look speed |
| Level view | On | The right stick's up / down does not tilt your view on foot |
| Joystick Deadzone | 0.15 | How far a stick moves before it counts |
| Motion Controls Inactivity Timer | 30 s | No button pressed this long: the controllers count as idle |
| Alternative Joy Layout | Off | See above |
| Swap Controller Inputs | Off | See above |
| Temple gestures | On | Flashlight and scanner at your temples |
| Grips as LB / RB in the scanner, star map and build mode | On | Off: the grips grab as usual there |
| Impact melee / instant hits | On / On | Swing to hit; swing speed 2.2 m/s |
| Throw your melee weapon | On | Grip, swing, let go |
| Throwing: lock onto the person your hand points at | On | The throw flies at them |
| Gravity gloves | On | Reach 5 m |
| Gloves point with | Your hand | Or your palm, or sighting along your hand |
| Powers (palm push) | On | Push speed 1.5 m/s |
| Grenades from behind your shoulder | On | |
| Dual wielding | On | The left trigger's choice in the inventory |
| Favorites wheel | On | Off: the game's own favorites menu |
| Climbing | On | Pull 1.0x |
| Ladders in first person | On | Climb with your hands |
| Hand interaction | On | Reach 0.35 m from the controller |
| Menus: the left thumbstick navigates | On | |
| Trading: the left thumbstick switches tabs | On | |
| Conversations: point and pull the trigger | On | |
| Conversations: press with your finger | Off | |
| Conversations: the right stick turns you | On | |
| Point and click with the controllers (Menus and Virtual Screen) | On | |
