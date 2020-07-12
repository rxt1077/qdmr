#include "opengd77_codeplug.hh"
#include "config.hh"
#include "channel.hh"
#include "utils.hh"
#include "logger.hh"
#include <QDateTime>


#define OFFSET_TIMESTMP     0x00088
#define OFFSET_SETTINGS     0x000e0
#define OFFSET_MSGTAB       0x00128
#define OFFSET_SCANTAB      0x01790
#define OFFSET_BANK_0       0x03780 // Channels 1-128
#define OFFSET_INTRO        0x07540
#define OFFSET_ZONETAB      0x08010
#define OFFSET_BANK_1       0x7b1b0 // Channels 129-1024
#define OFFSET_CONTACTS     0x87620
#define OFFSET_GROUPTAB     0x8d620

#define GET_MSGTAB()        ((msgtab_t*) &radio_mem[OFFSET_MSGTAB])


/* ******************************************************************************************** *
 * Implementation of GD77Codeplug
 * ******************************************************************************************** */
OpenGD77Codeplug::OpenGD77Codeplug(QObject *parent)
  : GD77Codeplug(parent)
{
  remImage(0);

  addImage("OpenGD77 Codeplug EEPROM");
  image(0).addElement(0x000e0, 0x05f20);
  image(0).addElement(0x07500, 0x03b00);
  addImage("OpenGD77 Codeplug FLASH");
  image(0).addElement(0x00000, 0x011a0);
  image(0).addElement(0x7b000, 0x13e60);
}

bool
OpenGD77Codeplug::encode(Config *config) {
  // set timestamp
  timestamp_t *ts = (timestamp_t *)data(OFFSET_TIMESTMP);
  ts->setNow();

  // pack basic config
  general_settings_t *gs = (general_settings_t*) data(OFFSET_SETTINGS);
  gs->initDefault();
  gs->setName(config->name());
  gs->setRadioId(config->id());

  intro_text_t *it = (intro_text_t*) data(OFFSET_INTRO);
  it->setIntroLine1(config->introLine1());
  it->setIntroLine2(config->introLine2());

  // Pack channels
  for (int i=0; i<NCHAN; i++) {
    // First, get bank
    bank_t *b;
    if ((i>>7) == 0)
      b = (bank_t*) data(OFFSET_BANK_0);
    else
      b = (((i>>7)-1) + (bank_t *) data(OFFSET_BANK_1));
    channel_t *ch = &b->chan[i % 128];

    // Disable channel if not used
    if (i >= config->channelList()->count()) {
      b->bitmap[(i % 128) / 8] &= ~(1 << (i & 7));
      continue;
    }

    // Construct from Channel object
    ch->clear();
    ch->fromChannelObj(config->channelList()->channel(i), config);

    // Set valid bit.
    b->bitmap[(i % 128) / 8] |= (1 << (i & 7));
  }

  // Pack Zones
  bool pack_zone_a = true;
  for (int i=0, j=0; i<NZONES; i++) {
    zonetab_t *zt = (zonetab_t*) data(OFFSET_ZONETAB);
    zone_t *z = &zt->zone[i];

next:
    if (j >= config->zones()->count()) {
      // Clear valid bit.
      zt->bitmap[i / 8] &= ~(1 << (i & 7));
      continue;
    }

    // Construct from Zone obj
    Zone *zone = config->zones()->zone(j);
    if (pack_zone_a) {
      pack_zone_a = false;
      if (zone->A()->count())
        z->fromZoneObjA(zone, config);
      else
        goto next;
    } else {
      pack_zone_a = true;
      j++;
      if (zone->B()->count())
        z->fromZoneObjB(zone, config);
      else
        goto next;
    }

    // Set valid bit.
    zt->bitmap[i / 8] |= (1 << (i & 7));
  }

  // Pack Scanlists
  for (int i=0; i<NSCANL; i++) {
    scantab_t *st = (scantab_t*) data(OFFSET_SCANTAB);
    scanlist_t *sl = &st->scanlist[i];

    if (i >= config->scanlists()->count()) {
      // Clear valid bit.
      st->valid[i] = 0;
      continue;
    }

    sl->fromScanListObj(config->scanlists()->scanlist(i), config);
    st->valid[i] = 1;
  }

  // Pack contacts
  for (int i=0; i<NCONTACTS; i++) {
    contact_t *ct = (contact_t*) data(OFFSET_CONTACTS + (i)*sizeof(contact_t));
    ct->clear();
    if (i >= config->contacts()->digitalCount())
      continue;
    ct->fromContactObj(config->contacts()->digitalContact(i), config);
  }

  // Pack Grouplists:
  for (int i=0; i<NGLISTS; i++) {
    grouptab_t *gt = (grouptab_t*) data(OFFSET_GROUPTAB);
    grouplist_t *gl = &gt->grouplist[i];
    if (i >= config->rxGroupLists()->count()) {
      gt->nitems1[i] = 0;
      continue;
    }
    // Enable group list
    gt->nitems1[i] = std::min(15,config->rxGroupLists()->list(i)->count()) + 1;
    gl->fromRXGroupListObj(config->rxGroupLists()->list(i), config);
  }

  return true;
}

bool
OpenGD77Codeplug::decode(Config *config) {
  // Clear config object
  config->reset();

  /* Unpack general config */
  general_settings_t *gs = (general_settings_t*) data(OFFSET_SETTINGS);
  if (nullptr == gs) {
    _errorMessage = QString("%1(): Cannot access general settings memory!")
        .arg(__func__);
    return false;
  }

  config->setId(gs->getRadioId());
  config->setName(gs->getName());
  intro_text_t *it = (intro_text_t*) data(OFFSET_INTRO);
  config->setIntroLine1(it->getIntroLine1());
  config->setIntroLine2(it->getIntroLine2());

  /* Unpack Contacts */
  for (int i=0; i<NCONTACTS; i++) {
    contact_t *ct = (contact_t *) data(OFFSET_CONTACTS+i*sizeof(contact_t));
    if (nullptr == ct) {
      _errorMessage = QString("%1(): Cannot access contact memory at index %2!")
          .arg(__func__).arg(i);
      return false;
    }
    // If contact is disabled
    if (! ct->isValid())
      continue;
    logDebug() << "Contact " << i << " enabled.";
    if (DigitalContact *cont = ct->toContactObj()) {
      config->contacts()->addContact(cont);
    } else {
      _errorMessage = QString("%1(): Cannot decode codeplug: Invalid contact at index %2.")
          .arg(__func__).arg(i);
      return false;
    }
  }

  /* Unpack RX Group Lists */
  for (int i=0; i<NGLISTS; i++) {
    grouptab_t *gt = (grouptab_t*) data(OFFSET_GROUPTAB);
    if (nullptr == gt) {
      _errorMessage = QString("%1(): Cannot access group list table memory!")
          .arg(__func__);
      return false;
    }
    if (0 == gt->nitems1[i])
      continue;
    grouplist_t *gl = &gt->grouplist[i];
    if (nullptr == gl) {
      _errorMessage = QString("%1(): Cannot access group list memory at index %2!")
          .arg(__func__).arg(i);
      return false;
    }
    RXGroupList *list = gl->toRXGroupListObj();
    if (list) {
      config->rxGroupLists()->addList(list);
    } else {
      _errorMessage = QString("%1(): Cannot decode codeplug: Invalid RX group-list at index %2.")
          .arg(__func__).arg(i);
      return false;
    }
    if(! gl->linkRXGroupListObj(list, config)) {
      _errorMessage = QString("%1(): Cannot decode codeplug: Cannot link RX group list at index %2.")
          .arg(__func__).arg(i);
      return false;
    }
  }

  /* Unpack Channels */
  for (int i=0; i<NCHAN; i++) {
    // First, get bank
    bank_t *b;
    if ((i>>7) == 0)
      b = (bank_t*) data(OFFSET_BANK_0);
    else
      b = (((i>>7)-1) + (bank_t*) data(OFFSET_BANK_1));
    if (nullptr == b) {
      _errorMessage = QString("%1(): Cannot access channel bank at index %2!")
          .arg(__func__).arg(i);
      return false;
    }
    // If channel is disabled -> skip
    if (! ((b->bitmap[i % 128 / 8] >> (i & 7)) & 1) )
      continue;
    // finally, get channel
    channel_t *ch = &b->chan[i % 128];
    if (! ch){
      _errorMessage = QString("%1(): Cannot access channel at index %2!")
          .arg(__func__).arg(i);
      return false;
    }
    if (Channel *chan = ch->toChannelObj()) {
      config->channelList()->addChannel(chan);
    } else {
      _errorMessage = QString("%1(): Cannot unpack codeplug: Invalid channel at index %2!")
          .arg(__func__).arg(i);
      return false;
    }
  }

  /* Unpack Zones */
  for (int i=0; i<NZONES; i++) {
    zonetab_t *zt = (zonetab_t*) data(OFFSET_ZONETAB);
    if (! zt){
      _errorMessage = QString("%1(): Cannot access zone table memory.")
          .arg(__func__);
      return false;
    }
    // if zone is disabled
    if (! (zt->bitmap[i / 8] >> (i & 7) & 1) )
      continue;
    // get zone_t
    zone_t *z = &zt->zone[i];
    if (! z){
      _errorMessage = QString("%1(): Cannot access zone at index %2")
          .arg(__func__).arg(i);
      return false;
    }

    // Create & link zone obj
    Zone *zone = z->toZoneObj();
    if (nullptr == zone) {
      _errorMessage = QString("%1(): Cannot unpack codeplug: Invalid zone at index %2")
          .arg(__func__).arg(i);
      return false;
    }
    if (! z->linkZoneObj(zone, config)) {
      _errorMessage = QString("%1(): Cannot unpack codeplug: Cannot link zone at index %2")
          .arg(__func__).arg(i);
      return false;
    }
    config->zones()->addZone(zone);
  }

  /* Unpack Scan lists */
  for (int i=0; i<NSCANL; i++) {
    scantab_t *st = (scantab_t*) data(OFFSET_SCANTAB);
    if (! st){
      _errorMessage = QString("%1(): Cannot access scanlist table memory!")
          .arg(__func__);
      return false;
    }
    if (! st->valid[i])
      continue;
    scanlist_t *sl = &st->scanlist[i];
    if (! sl){
      _errorMessage = QString("%1(): Cannot access scan list at index %2")
          .arg(__func__).arg(i);
      return false;
    }
    ScanList *scan = sl->toScanListObj();
    if (nullptr == scan) {
      _errorMessage = QString("%1(): Cannot unpack codeplug: Invalid scanlist at index %2")
          .arg(__func__).arg(i);
      return false;
    }
    if (! sl->linkScanListObj(scan, config)) {
      _errorMessage = QString("%1(): Cannot unpack codeplug: Cannot link scanlist at index %2")
          .arg(__func__).arg(i);
      return false;
    }
    config->scanlists()->addScanList(scan);
  }

  /*
   * Link Channels -> ScanLists
   */
  for (int i=0; i<NCHAN; i++) {
    // First, get bank
    bank_t *b;
    if ((i>>7) == 0)
      b = (bank_t*) data(OFFSET_BANK_0);
    else
      b = (((i>>7)-1) + (bank_t*) data(OFFSET_BANK_1));
    // If channel is disabled
    if (! ((b->bitmap[i % 128 / 8] >> (i & 7)) & 1) )
      continue;
    // finally, get channel
    channel_t *ch = &b->chan[i % 128];
    if (! ch->linkChannelObj(config->channelList()->channel(i), config)) {
      _errorMessage = QString("%1(): Cannot unpack codeplug: Cannot link channel at index %2")
          .arg(__func__).arg(i);
      return false;
    }
  }

  return true;
}
