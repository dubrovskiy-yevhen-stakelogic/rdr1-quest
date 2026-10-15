package org.rdrvr.bootstrap;

/** Process-local verification and per-Activity launch policy, also tested on the host JVM. */
final class StartupGate {
    enum State { VERIFYING, READY, FAILED }
    enum Action { WAIT, LAUNCH, ERROR, NONE }
    static final class Readiness {
        private State state=State.VERIFYING;
        synchronized State get() {return state;}
        synchronized void ready() {if(state==State.VERIFYING)state=State.READY;}
        synchronized void fail() {state=State.FAILED;}
    }
    private boolean resumed,launched;
    StartupGate(boolean launched) {this.launched=launched;}
    void resume() {resumed=true;}
    void pause() {resumed=false;}
    boolean launched() {return launched;}
    Action next(State state) {
        if(!resumed || launched)return Action.NONE;
        if(state==State.FAILED)return Action.ERROR;
        if(state!=State.READY)return Action.WAIT;
        launched=true;return Action.LAUNCH;
    }
}
