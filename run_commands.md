# Detailing build caveats and node placement for the demo run.
NS-3’s own GUI tool is NetAnim, the Qt-based animator that reads an XML trace file the simulation writes. The other NS-3 GUI, PyViz, needs the old Python bindings that, as far as I know, ns-3.38 no longer ships, so NetAnim is the one to use. I’ll add it to the project as an option, switched on with --anim=1.
Ran 2 commands, shared files
I added NetAnim support to the project. It is not compiled or run yet, because I can’t build NS-3 here, so treat it as unverified until it builds on your machine.

Two files are attached:

dynamic-slicing.cc: the full source with the NetAnim code already in it.
add_netanim.py: a small script that adds the same code to the file you already have.
## 1. Add the GUI code to your simulation
Either replace your file with the attached dynamic-slicing.cc, or patch the file you have:

bash
```
python3 ~/5g-network-slicing/scripts/add_netanim.py ~/ns-3-dev/scratch/dynamic-slicing.cc
cd ~/ns-3-dev && ./ns3 build
```
## 2. Install the NetAnim viewer (once)
```
NetAnim is a separate Qt program, not part of the NS-3 build:

bash
sudo apt install -y qt5-default git make g++
cd ~
git clone https://gitlab.com/nsnam/netanim.git
cd netanim
git tag | tail -3                  # pick the newest tag, e.g. netanim-3.109
git checkout netanim-3.109         # use the tag you actually see
qmake NetAnim.pro
make -j2
```
This produces the ~/netanim/NetAnim program. It needs a graphical desktop session, which your VirtualBox Ubuntu has.

## 3. Run the simulation and write the animation file
Use a short run and a separate output folder, so the animation test doesn’t mix into your real results/ CSVs:
bash
```
cd ~/ns-3-dev
P=~/5g-network-slicing
./ns3 run "dynamic-slicing --enableDynamicSlicing=1 --seed=1 --simTime=15 \
  --anim=1 --animFile=$P/results/slicing-anim.xml --animStart=9 --animStop=13 \
  --outDir=/tmp/animrun --configFile=$P/config/simulation-config.txt"
```
--animStart and --animStop are absolute simulation seconds. The window 9–13 s covers the eMBB spike, which starts at 10.5 s.
A full 50 s trace with packets would make a huge XML file. If the file is still too large, add --animPackets=0, which stores nodes and counters only.
4. Open it in the GUI
bash
```
~/netanim/NetAnim
```
Click the open-file button (File → Open XML trace file) and select ~/5g-network-slicing/results/slicing-anim.xml. Then press the play button.

## What you will see
Topology: RemoteHost, then the 5G core (PGW), then the gNB, then the UEs in a ring around it. The UEs are coloured by slice (blue eMBB, red URLLC, green mMTC) and labelled like eMBB-UE0.
Packets moving on the RemoteHost–PGW and PGW–gNB links.
Node counters (a table view in NetAnim; the name varies by version) with each UE’s slice share in % and its throughput in Mbps, updated once per second. This is where the dynamic reallocation becomes visible: run with --enableDynamicSlicing=1 and watch the share values change after the traffic phase changes.
Limitation to tell your professor
NetAnim draws packets only for device types it knows. As far as I know, it has no tracing for the NR radio devices. I expect the wired core links to show animated packets, but the radio hop between the gNB and the UEs probably won’t show packets. That is why I added the counters on the UEs, so the radio side still shows something live. I haven’t been able to confirm this behavior, so test it and tell me what you see. If you want a stronger visual for the radio side, the next step is to add more counters or a moving marker for each slice.
