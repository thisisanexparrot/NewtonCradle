/*
 * Sifteo SDK Example.
 */

#include <sifteo.h>
#include "assets.gen.h"
using namespace Sifteo;

static Metadata M = Metadata()
    .title("Sensors SDK Example")
    .package("com.sifteo.sdk.sensors", "1.1")
    .icon(Icon)
    .cubeRange(0, CUBE_ALLOCATION);

AssetSlot gMainSlot = AssetSlot::allocate()
  .bootstrap(BootstrapAssets);

static VideoBuffer vbuf[CUBE_ALLOCATION];
static TiltShakeRecognizer motion[CUBE_ALLOCATION];

static CubeSet newCubes; // new cubes as a result of paint()
static CubeSet lostCubes; // lost cubes as a result of paint()
static CubeSet reconnectedCubes; // reconnected (lost->new) cubes as a result of paint()
static CubeSet dirtyCubes; // dirty cubes as a result of paint()
static CubeSet activeCubes; // cubes showing the active scene

static AssetLoader loader; // global asset loader (each cube will have symmetric assets)
static AssetConfiguration<1> config; // global asset configuration (will just hold the bootstrap group)

class SensorListener {
public:
    struct Counter {
        unsigned touch;
        unsigned neighborAdd;
        unsigned neighborRemove;
    } counters[CUBE_ALLOCATION];

    void install()
    {
        Events::neighborAdd.set(&SensorListener::onNeighborAdd, this);
        Events::neighborRemove.set(&SensorListener::onNeighborRemove, this);
        Events::cubeAccelChange.set(&SensorListener::onAccelChange, this);
        Events::cubeTouch.set(&SensorListener::onTouch, this);
        Events::cubeBatteryLevelChange.set(&SensorListener::onBatteryChange, this);
        Events::cubeConnect.set(&SensorListener::onConnect, this);

        // Handle already-connected cubes
        for (CubeID cube : CubeSet::connected())
            onConnect(cube);
    }

private:
    void onConnect(unsigned id)
    {
        CubeID cube(id);
        uint64_t hwid = cube.hwID();

        bzero(counters[id]);
        LOG("Cube %d connected\n", id);

        vbuf[id].initMode(BG0_ROM);
        vbuf[id].attach(id);
        motion[id].attach(id);

        // Draw the cube's identity
        String<128> str;
        str << "I am cube #" << cube << "\n";
        str << "hwid " << Hex(hwid >> 32) << "\n     " << Hex(hwid) << "\n\n";
        vbuf[cube].bg0rom.text(vec(1,2), str);

        // Draw initial state for all sensors
        onAccelChange(cube);
        onBatteryChange(cube);
        onTouch(cube);
        drawNeighbors(cube);
    }

    void onBatteryChange(unsigned id)
    {
        CubeID cube(id);
        String<32> str;
        str << "bat:   " << FixedFP(cube.batteryLevel(), 1, 3) << "\n";
        vbuf[cube].bg0rom.text(vec(1,13), str);
    }

    void onTouch(unsigned id)
    {
        CubeID cube(id);
        counters[id].touch++;
        LOG("Touch event on cube #%d, state=%d\n", id, cube.isTouching());

        String<32> str;
        str << "touch: " << cube.isTouching() <<
            " (" << counters[cube].touch << ")\n";
        vbuf[cube].bg0rom.text(vec(1,9), str);
    }

    void onAccelChange(unsigned id)
    {
        CubeID cube(id);
        auto accel = cube.accel();

        String<64> str;
        str << "acc: "
            << Fixed(accel.x, 3)
            << Fixed(accel.y, 3)
            << Fixed(accel.z, 3) << "\n";

        unsigned changeFlags = motion[id].update();
        if (changeFlags) {
            // Tilt/shake changed

            LOG("Tilt/shake changed, flags=%08x\n", changeFlags);

            auto tilt = motion[id].tilt;
            str << "tilt:"
                << Fixed(tilt.x, 3)
                << Fixed(tilt.y, 3)
                << Fixed(tilt.z, 3) << "\n";

            str << "shake: " << motion[id].shake;
        }

        vbuf[cube].bg0rom.text(vec(1,10), str);
    }

    void onNeighborRemove(unsigned firstID, unsigned firstSide, unsigned secondID, unsigned secondSide)
    {
        LOG("Neighbor Remove: %02x:%d - %02x:%d\n", firstID, firstSide, secondID, secondSide);

        if (firstID < arraysize(counters)) {
            counters[firstID].neighborRemove++;
            drawNeighbors(firstID);
        }
        if (secondID < arraysize(counters)) {
            counters[secondID].neighborRemove++;
            drawNeighbors(secondID);
        }
    }

    void onNeighborAdd(unsigned firstID, unsigned firstSide, unsigned secondID, unsigned secondSide)
    {
        LOG("Neighbor Add: %02x:%d - %02x:%d\n", firstID, firstSide, secondID, secondSide);

        if (firstID < arraysize(counters)) {
            counters[firstID].neighborAdd++;
            drawNeighbors(firstID);
        }
        if (secondID < arraysize(counters)) {
            counters[secondID].neighborAdd++;
            drawNeighbors(secondID);
        }
    }

    void drawNeighbors(CubeID cube)
    {
        Neighborhood nb(cube);

        String<64> str;
        str << "nb "
            << Hex(nb.neighborAt(TOP), 2) << " "
            << Hex(nb.neighborAt(LEFT), 2) << " "
            << Hex(nb.neighborAt(BOTTOM), 2) << " "
            << Hex(nb.neighborAt(RIGHT), 2) << "\n";

        str << "   +" << counters[cube].neighborAdd
            << ", -" << counters[cube].neighborRemove
            << "\n\n";

        BG0ROMDrawable &draw = vbuf[cube].bg0rom;
        draw.text(vec(1,6), str);

        drawSideIndicator(draw, nb, vec( 1,  0), vec(14,  1), TOP);
        drawSideIndicator(draw, nb, vec( 0,  1), vec( 1, 14), LEFT);
        drawSideIndicator(draw, nb, vec( 1, 15), vec(14,  1), BOTTOM);
        drawSideIndicator(draw, nb, vec(15,  1), vec( 1, 14), RIGHT);
    }

    static void drawSideIndicator(BG0ROMDrawable &draw, Neighborhood &nb,
        Int2 topLeft, Int2 size, Side s)
    {
        unsigned nbColor = draw.ORANGE;
        draw.fill(topLeft, size,
            nbColor | (nb.hasNeighborAt(s) ? draw.SOLID_FG : draw.SOLID_BG));
    }
};

static void playSfx(const AssetAudio& sfx) {
  static int i=0;
  AudioChannel(i).play(sfx);
  i = 1 - i;
}

static Int2 getRestPosition(Side s) {
  // Look up the top-left pixel of the bar for the given side.
  // We use a switch so that the compiler can optimize this
  // however if feels is best.
  switch(s) {
  case TOP: return vec(64 - Bars[0].pixelWidth()/2,0);
  case LEFT: return vec(0, 64 - Bars[1].pixelHeight()/2);
  case BOTTOM: return vec(64 - Bars[2].pixelWidth()/2, 128-Bars[2].pixelHeight());
  case RIGHT: return vec(128-Bars[3].pixelWidth(), 64 - Bars[3].pixelHeight()/2);
  default: return vec(0,0);
  }
}

static int barSpriteCount(CubeID cid) {
  // how many bars are showing on this cube?
  ASSERT(activeCubes.test(cid));
  int result = 0;
  for(int i=0; i<4; ++i) {
    if (!vbuf[cid].sprites[i].isHidden()) {
      result++;
    }
  }
  return result;
}

static bool showSideBar(CubeID cid, Side s) {
  // if cid is not showing a bar on side s, show it and check if the
  // smiley should wake up
  ASSERT(activeCubes.test(cid));
  if (vbuf[cid].sprites[s].isHidden()) {
    vbuf[cid].sprites[s].setImage(Bars[s]);
    vbuf[cid].sprites[s].move(getRestPosition(s));
    if (barSpriteCount(cid) == 1) {
      vbuf[cid].bg0.image(vec(0,0), Backgrounds, 1);
    }
    return true;
  } else {
    return false;
  }
}

static bool hideSideBar(CubeID cid, Side s) {
  // if cid is showing a bar on side s, hide it and check if the
  // smiley should go to sleep
  ASSERT(activeCubes.test(cid));
  if (!vbuf[cid].sprites[s].isHidden()) {
    vbuf[cid].sprites[s].hide();
    if (barSpriteCount(cid) == 0) {
      vbuf[cid].bg0.image(vec(0,0), Backgrounds, 0);
    }
    return true;
  } else {
    return false;
  }
}

static void activateCube(CubeID cid) {
  // mark cube as active and render its canvas
  activeCubes.mark(cid);
  vbuf[cid].initMode(BG0_SPR_BG1);
  vbuf[cid].bg0.image(vec(0,0), Backgrounds, 0);
  auto neighbors = vbuf[cid].physicalNeighbors();
  for(int side=0; side<4; ++side) {
    if (neighbors.hasNeighborAt(Side(side))) {
      showSideBar(cid, Side(side));
    } else {
      hideSideBar(cid, Side(side));
    }
  }
}

static void paintWrapper() {
  // clear the palette
  newCubes.clear();
  lostCubes.clear();
  reconnectedCubes.clear();
  dirtyCubes.clear();

  // fire events
  System::paint();

  // dynamically load assets just-in-time
  if (!(newCubes | reconnectedCubes).empty()) {
    AudioTracker::pause();
    playSfx(SfxConnect);
    loader.start(config);
    while(!loader.isComplete()) {
      for(CubeID cid : (newCubes | reconnectedCubes)) {
	vbuf[cid].bg0rom.hBargraph(
				   vec(0, 4), loader.cubeProgress(cid, 128), BG0ROMDrawable::ORANGE, 8
				   );
      }
      // fire events while we wait
      System::paint();
    }
    loader.finish();
    AudioTracker::resume();
  }

  // repaint cubes
  for(CubeID cid : dirtyCubes) {
    activateCube(cid);
  }

  // also, handle lost cubes, if you so desire :)
}

static void onCubeConnect(void* ctxt, unsigned cid) {
  // this cube is either new or reconnected
  if (lostCubes.test(cid)) {
    // this is a reconnected cube since it was already lost this paint()
    lostCubes.clear(cid);
    reconnectedCubes.mark(cid);
  } else {
    // this is a brand-spanking new cube
    newCubes.mark(cid);
  }
  // begin showing some loading art (have to use BG0ROM since we don't have assets)
  dirtyCubes.mark(cid);
  auto& g = vbuf[cid];
  g.attach(cid);
  g.initMode(BG0_ROM);
  g.bg0rom.fill(vec(0,0), vec(16,16), BG0ROMDrawable::SOLID_BG);
  g.bg0rom.text(vec(1,1), "Hold on!", BG0ROMDrawable::BLUE);
  g.bg0rom.text(vec(1,14), "Adding Cube...", BG0ROMDrawable::BLUE);
}

static void onCubeDisconnect(void* ctxt, unsigned cid) {
  // mark as lost and clear from other cube sets
  lostCubes.mark(cid);
  newCubes.clear(cid);
  reconnectedCubes.clear(cid);
  dirtyCubes.clear(cid);
  activeCubes.clear(cid);
}

static void onCubeRefresh(void* ctxt, unsigned cid) {
  // mark this cube for a future repaint
  dirtyCubes.mark(cid);
}

static bool isActive(NeighborID nid) {
  // Does this nid indicate an active cube?
  return nid.isCube() && activeCubes.test(nid);
}

static void onNeighborAdd(void* ctxt, unsigned cube0, unsigned side0, unsigned cube1, unsigned side1) {
  // update art on active cubes (not loading cubes or base)
  bool sfx = false;
  if (isActive(cube0)) { sfx |= showSideBar(cube0, Side(side0)); }
  if (isActive(cube1)) { sfx |= showSideBar(cube1, Side(side1)); }
  if (sfx) { playSfx(SfxAttach); }
}

static void onNeighborRemove(void* ctxt, unsigned cube0, unsigned side0, unsigned cube1, unsigned side1) {
  // update art on active cubes (not loading cubes or base)
  bool sfx = false;
  if (isActive(cube0)) { sfx |= hideSideBar(cube0, Side(side0)); }
  if (isActive(cube1)) { sfx |= hideSideBar(cube1, Side(side1)); }
  if (sfx) { playSfx(SfxDetach); }
}


void main()
{
    static SensorListener sensors;

    sensors.install();

    // initialize asset configuration and loader
    config.append(gMainSlot, BootstrapAssets);
    loader.init();

    // subscribe to events
    Events::cubeConnect.set(onCubeConnect);
    Events::cubeDisconnect.set(onCubeDisconnect);
    Events::cubeRefresh.set(onCubeRefresh);

    Events::neighborAdd.set(onNeighborAdd);
    Events::neighborRemove.set(onNeighborRemove);

    // initialize cubes
    AudioTracker::setVolume(0.2f * AudioChannel::MAX_VOLUME);
    AudioTracker::play(Music);
    for(CubeID cid : CubeSet::connected()) {
      vbuf[cid].attach(cid);
      activateCube(cid);
    }

    // We're entirely event-driven. Everything is
    // updated by SensorListener's event callbacks.
    while (1) {
        System::paint();
        paintWrapper();
    }
}
