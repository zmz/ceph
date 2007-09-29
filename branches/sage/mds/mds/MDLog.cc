// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "MDLog.h"
#include "MDS.h"
#include "MDCache.h"
#include "LogEvent.h"

#include "osdc/Journaler.h"

#include "common/LogType.h"
#include "common/Logger.h"

#include "events/ESubtreeMap.h"

#include "config.h"

#define  dout(l)    if (l<=g_conf.debug_mds || l <= g_conf.debug_mds_log) *_dout << dbeginl << g_clock.now() << " mds" << mds->get_nodeid() << ".log "
#define  derr(l)    if (l<=g_conf.debug_mds || l <= g_conf.debug_mds_log) *_derr << dbeginl << g_clock.now() << " mds" << mds->get_nodeid() << ".log "

// cons/des

LogType mdlog_logtype;


MDLog::~MDLog()
{
  if (journaler) { delete journaler; journaler = 0; }
  if (logger) { delete logger; logger = 0; }
}


void MDLog::reopen_logger(utime_t start, bool append)
{
  // logger
  char name[80];
  sprintf(name, "mds%d.log", mds->get_nodeid());
  logger = new Logger(name, &mdlog_logtype, append);
  logger->set_start(start);

  static bool didit = false;
  if (!didit) {
    didit = true;
    mdlog_logtype.add_inc("evadd");
    mdlog_logtype.add_inc("evtrm");
    mdlog_logtype.add_set("ev");
    mdlog_logtype.add_inc("segadd");
    mdlog_logtype.add_inc("segtrm");
    mdlog_logtype.add_set("segtrmg");    
    mdlog_logtype.add_set("seg");
    mdlog_logtype.add_set("expos");
    mdlog_logtype.add_set("wrpos");
    mdlog_logtype.add_avg("jlat");
  }

}

void MDLog::init_journaler()
{
  // inode
  memset(&log_inode, 0, sizeof(log_inode));
  log_inode.ino = MDS_INO_LOG_OFFSET + mds->get_nodeid();
  log_inode.layout = g_OSD_MDLogLayout;
  
  if (g_conf.mds_local_osd) 
    log_inode.layout.preferred = mds->get_nodeid() + g_conf.mds_local_osd_offset;  // hack
  
  // log streamer
  if (journaler) delete journaler;
  journaler = new Journaler(log_inode, mds->objecter, logger, &mds->mds_lock);
}

void MDLog::write_head(Context *c) 
{
  journaler->write_head(c);
}

off_t MDLog::get_read_pos() 
{
  return journaler->get_read_pos(); 
}

off_t MDLog::get_write_pos() 
{
  return journaler->get_write_pos(); 
}



void MDLog::create(Context *c)
{
  dout(5) << "create empty log" << dendl;
  init_journaler();
  journaler->reset();
  write_head(c);

  logger->set("expos", journaler->get_expire_pos());
  logger->set("wrpos", journaler->get_write_pos());
}

void MDLog::open(Context *c)
{
  dout(5) << "open discovering log bounds" << dendl;
  init_journaler();
  journaler->recover(c);

  // either append() or replay() will follow.
}

void MDLog::append()
{
  dout(5) << "append positioning at end" << dendl;
  journaler->set_read_pos(journaler->get_write_pos());
  journaler->set_expire_pos(journaler->get_write_pos());

  logger->set("expos", new_expire_pos);
}



// -------------------------------------------------

void MDLog::submit_entry( LogEvent *le, Context *c ) 
{
  if (!g_conf.mds_log) {
    // hack: log is disabled.
    if (c) {
      c->finish(0);
      delete c;
    }
    return;
  }

  dout(5) << "submit_entry " << journaler->get_write_pos() << " : " << *le << dendl;
  
  // let the event register itself in the segment
  assert(!segments.empty());
  le->_segment = segments.rbegin()->second;
  le->_segment->num_events++;
  le->update_segment();
  
  num_events++;
  assert(!capped);
  
  // encode it, with event type
  {
    bufferlist bl;
    bl.append((char*)&le->_type, sizeof(le->_type));
    le->encode_payload(bl);
    
    // journal it.
    journaler->append_entry(bl);  // bl is destroyed.
  }
  
  delete le;
  
  if (logger) {
    logger->inc("evadd");
    logger->set("ev", num_events);
    logger->set("wrpos", journaler->get_write_pos());
  }
  
  if (c) {
    unflushed = 0;
    journaler->flush(c);
  }
  else
    unflushed++;
  
  // start a new segment?
  //  FIXME: should this go elsewhere?
  off_t last_seg = get_last_segment_offset();
  if (!segments.empty() && 
      !writing_subtree_map &&
      (journaler->get_write_pos() / log_inode.layout.period()) != (last_seg / log_inode.layout.period()) &&
      (journaler->get_write_pos() - last_seg > log_inode.layout.period()/2)) {
    dout(10) << "submit_entry also starting new segment: last = " << last_seg
	     << ", cur pos = " << journaler->get_write_pos() << dendl;
    start_new_segment();
  }
}

void MDLog::wait_for_sync( Context *c )
{
  if (g_conf.mds_log) {
    // wait
    journaler->flush(c);
  } else {
    // hack: bypass.
    c->finish(0);
    delete c;
  }
}

void MDLog::flush()
{
  if (unflushed)
    journaler->flush();
  unflushed = 0;

  // trim
  trim();
}

void MDLog::cap()
{ 
  dout(5) << "cap" << dendl;
  capped = true;
}


// -----------------------------
// segments

void MDLog::start_new_segment(Context *onsync)
{
  dout(7) << "start_new_segment at " << journaler->get_write_pos() << dendl;
  assert(!writing_subtree_map);

  segments[journaler->get_write_pos()] = new LogSegment(journaler->get_write_pos());

  writing_subtree_map = true;

  ESubtreeMap *le = mds->mdcache->create_subtree_map();
  submit_entry(le, new C_MDL_WroteSubtreeMap(this, mds->mdlog->get_write_pos()));
  if (onsync)
    wait_for_sync(onsync);  

  logger->inc("segadd");
  logger->set("seg", segments.size());
}

void MDLog::_logged_subtree_map(off_t off)
{
  dout(10) << "_logged_subtree_map at " << off << dendl;
  writing_subtree_map = false;

  /*
  list<Context*> ls;
  take_subtree_map_expire_waiters(ls);
  mds->queue_waiters(ls);
  */
}



void MDLog::trim()
{
  // trim!
  dout(10) << "trim " 
	   << segments.size() << " / " << max_segments << " segments, " 
	   << num_events << " / " << max_events << " events"
	   << ", " << trimming_segments.size() << " trimming"
	   << dendl;

  if (segments.empty()) return;

  // hack: only trim for a few seconds at a time
  utime_t stop = g_clock.now();
  stop += 2.0;

  map<off_t,LogSegment*>::iterator p = segments.begin();
  int left = num_events;
  while (p != segments.end() && 
	 ((max_events >= 0 && left > max_events) ||
	  (max_segments >= 0 && (int)(segments.size()-trimming_segments.size()) > max_segments))) {

    if (stop < g_clock.now())
      break;

    if ((int)trimming_segments.size() >= g_conf.mds_log_max_trimming)
      break;

    // look at first segment
    LogSegment *ls = p->second;
    assert(ls);

    p++;
    
    if (trimming_segments.count(ls)) {
      dout(5) << "trim already trimming segment " << ls->offset << ", " << ls->num_events << " events" << dendl;
    } else {
      try_trim(ls);
    }

    left -= ls->num_events;
  }
}


void MDLog::try_trim(LogSegment *ls)
{
  C_Gather *exp = ls->try_to_expire(mds);
  if (exp) {
    trimming_segments.insert(ls);
    dout(5) << "try_trim trimming segment " << ls->offset << dendl;
    exp->set_finisher(new C_MaybeTrimmedSegment(this, ls));
  } else {
    dout(10) << "try_trim trimmed segment " << ls->offset << dendl;
    _trimmed(ls);
  }
  
  logger->set("segtrmg", trimming_segments.size());
}

void MDLog::_maybe_trimmed(LogSegment *ls) 
{
  dout(10) << "_maybe_trimmed segment " << ls->offset << " " << ls->num_events << " events" << dendl;
  assert(trimming_segments.count(ls));
  trimming_segments.erase(ls);
  try_trim(ls);
}

void MDLog::_trimmed(LogSegment *ls)
{
  dout(5) << "_trimmed segment " << ls->offset << " " << ls->num_events << " events" << dendl;

  // don't trim last segment, unless we're capped
  if (!capped && ls == get_current_segment()) {
    dout(5) << "_trimmed not trimming " << ls->offset << ", last one and !capped" << dendl;
    return;
  }

  num_events -= ls->num_events;

  assert(segments.count(ls->offset));
  if (segments.begin()->second == ls) {
    journaler->set_expire_pos(ls->offset);  // this was the oldest segment, adjust expire pos
    logger->set("expos", ls->offset);
  }
  segments.erase(ls->offset);

  logger->set("ev", num_events);
  logger->inc("evtrm", ls->num_events);
  logger->set("seg", segments.size());
  logger->inc("segtrm");

  delete ls;
}



void MDLog::replay(Context *c)
{
  assert(journaler->is_active());

  // start reading at the last known expire point.
  journaler->set_read_pos( journaler->get_expire_pos() );

  // empty?
  if (journaler->get_read_pos() == journaler->get_write_pos()) {
    dout(10) << "replay - journal empty, done." << dendl;
    if (c) {
      c->finish(0);
      delete c;
    }
    return;
  }

  // add waiter
  if (c)
    waitfor_replay.push_back(c);

  // go!
  dout(10) << "replay start, from " << journaler->get_read_pos()
	   << " to " << journaler->get_write_pos() << dendl;

  assert(num_events == 0);

  replay_thread.create();
  //_replay(); 
}

class C_MDL_Replay : public Context {
  MDLog *mdlog;
public:
  C_MDL_Replay(MDLog *l) : mdlog(l) {}
  void finish(int r) { 
    mdlog->replay_cond.Signal();
    //mdlog->_replay(); 
  }
};



// i am a separate thread
void MDLog::_replay_thread()
{
  mds->mds_lock.Lock();
  dout(10) << "_replay_thread start" << dendl;

  // loop
  off_t new_expire_pos = journaler->get_expire_pos();
  while (1) {
    // wait for read?
    while (!journaler->is_readable() &&
	   journaler->get_read_pos() < journaler->get_write_pos()) {
      journaler->wait_for_readable(new C_MDL_Replay(this));
      replay_cond.Wait(mds->mds_lock);
    }
    
    if (!journaler->is_readable() &&
	journaler->get_read_pos() == journaler->get_write_pos())
      break;
    
    assert(journaler->is_readable());
    
    // read it
    off_t pos = journaler->get_read_pos();
    bufferlist bl;
    bool r = journaler->try_read_entry(bl);
    assert(r);
    
    // unpack event
    LogEvent *le = LogEvent::decode(bl);

    // new segment?
    if (le->get_type() == EVENT_SUBTREEMAP) {
      segments[pos] = new LogSegment(pos);
      logger->set("seg", segments.size());
    }

    le->_segment = get_current_segment();    // replay may need this

    // have we seen an import map yet?
    if (segments.empty()) {
      dout(10) << "_replay " << pos << " / " << journaler->get_write_pos() 
	       << " -- waiting for subtree_map.  (skipping " << *le << ")" << dendl;
    } else {
      dout(10) << "_replay " << pos << " / " << journaler->get_write_pos() 
	       << " : " << *le << dendl;
      le->replay(mds);

      num_events++;
      if (!new_expire_pos) 
	new_expire_pos = pos;
    }
    delete le;

    logger->set("rdpos", pos);

    // drop lock for a second, so other events/messages (e.g. beacon timer!) can go off
    mds->mds_lock.Unlock();
    mds->mds_lock.Lock();
  }

  // done!
  assert(journaler->get_read_pos() == journaler->get_write_pos());
  dout(10) << "_replay - complete, " << num_events << " events, new read/expire pos is " << new_expire_pos << dendl;
  
  // move read pointer _back_ to first subtree map we saw, for eventual trimming
  journaler->set_read_pos(new_expire_pos);
  journaler->set_expire_pos(new_expire_pos);
  logger->set("expos", new_expire_pos);
  
  // kick waiter(s)
  list<Context*> ls;
  ls.swap(waitfor_replay);
  finish_contexts(ls,0);  
  
  dout(10) << "_replay_thread finish" << dendl;
  mds->mds_lock.Unlock();
}



