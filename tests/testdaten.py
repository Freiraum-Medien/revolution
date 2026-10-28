#!/usr/bin/env python3
# testdaten.py – Testdaten in die vier Testkonten einspielen (#344)
#
# Versand: SMTP über Camel mit den Transport-Quellen von privat@ und geops@.
# Ablage: Camel-Stores der vier Konten (imapx bzw. microsoft365), konfiguriert
# aus der ESourceRegistry wie in Evolution (e_source_camel_configure_service).
# Zugangsdaten nur zur Laufzeit aus GOA, nie ausgegeben, nie gespeichert.
#
# Aufruf:
#   tests/testdaten.py            einspielen (fehlende Teile ergänzen)
#   tests/testdaten.py --dry-run  nur anzeigen, was fehlt
#   tests/testdaten.py --pruefen  nur lesend belegen, was vorhanden ist
#   tests/testdaten.py --ordner   nur lesend: Ordnerliste aller Mail-Konten aus GOA (#348),
#                                 auch der nur lesend freigegebenen; Vergleich mit dem Log
#                                 der App (G_MESSAGES_DEBUG=revolution, „Ordner: UID Name“)
#   tests/testdaten.py --zaehler [LOG]  #350: nur lesend, unabhängig von Camel: ungelesene
#                                 Nachrichten je Ordner aller Mail-Konten aus GOA, IMAP per STATUS
#                                 (kein SELECT/EXAMINE, \\Recent bleibt), microsoft365 per Graph
#                                 (GET); Zeilen „ZAEHLER <UID> <Ordner> <N>“. Mit LOG (App-Log,
#                                 G_MESSAGES_DEBUG=revolution) Vergleich mit dem letzten
#                                 „Ungelesen: <UID> <Ordner> <N>“ je Ordner, Exit 1 bei Abweichung
#   tests/testdaten.py --last N   #352: N Lastmails per IMAP-APPEND (kein Versand) in den Ordner
#                                 „Revolution-Last“ von privat@ (fehlende ergänzen); letzte
#                                 Zeile „LAST <UID>/<voller Ordnername> <Anzahl>“; #350: mit
#                                 --last-ordner NAME in einen anderen Ordner (Abbruch-Test Zähler)
#   tests/testdaten.py --flags KONTO
#                                 #353, nur lesend, unabhängig von Camel: Flags der Mails im Ordner
#                                 Revolution-Test direkt vom Server (IMAP EXAMINE + UID FETCH FLAGS
#                                 bzw. Graph GET isRead/importance/flag); Zeilen „ORDNER <UID>/<Name>“
#                                 und „FLAGS <gelesen 0|1> <markiert 0|1> <Betreff>“
#   tests/testdaten.py --server KONTO AKTION
#                                 #353, schreibend nur in Testkonten: serverseitig an der Einladung
#                                 in Revolution-Test gelesen|ungelesen|markiert|unmarkiert (IMAP UID
#                                 STORE bzw. Graph PATCH), neu (IMAP APPEND einer Live-Mail, Zeile „BETREFF …“) oder
#                                 weg (Live-Mails löschen: UID STORE \Deleted + UID EXPUNGE je UID); #414: mit
#                                 --warte DATEI erst anmelden, „BEREIT“ ausgeben, dann ablegen,
#                                 sobald DATEI existiert, und „ABGELEGT <Uhrzeit>“ ausgeben; #416: mit
#                                 Papierkorb (nur Graph) an der Einladung im Papierkorb (tests/verschieben.sh)
#   tests/testdaten.py --testordner KONTO anlegen|loeschen
#                                 #408, schreibend nur in Testkonten (IMAP): Ordner Revolution-Offline
#                                 neben Revolution-Test per CREATE bzw. DELETE; Zeile
#                                 „TESTORDNER <Camel-Name>“
#   tests/testdaten.py --ungelesen KONTO ORDNER [--soll DATEI]
#                                 #353: Zeile „UNGELESEN <Anzahl> <UID …>“ (IMAP EXAMINE + UID SEARCH
#                                 UNSEEN, nur lesend); mit --soll (Datei mit einer solchen Zeile)
#                                 stellt es genau diesen Stand per UID STORE ±\Seen her (Testkonten)
#   tests/testdaten.py --anzahl UID ORDNER
#                                 #410: nur lesend, unabhängig von Camel, jedes Konto aus GOA: Anzahl
#                                 der Nachrichten eines IMAP-Ordners (EXAMINE, UID SEARCH UNDELETED
#                                 ohne Junk); Zeile „ANZAHL <UID>/<Ordner> <sichtbar> EXISTS <n>“;
#                                 #414: microsoft365 per Graph GET (totalItemCount, beide Zahlen gleich)
#   tests/testdaten.py --anzeige  #354: nur die Anzeige-Mails (HTML, Text ISO-8859-1, Inline-Bild
#                                 cid:, Remote-Inhalte; #355: zweite Remote-Mail und Remote-Mail eines
#                                 anderen Absenders) per APPEND in den Posteingang der Testkonten
#                                 außer privat@ (Quota, #411; fehlende ergänzen, kein Versand); im
#                                 Standardlauf enthalten
#   tests/testdaten.py --anhang   #356: Anhang-Mail (PDF, PNG, ZIP, Text mit „../“ im Dateinamen,
#                                 angehängte Nachricht) per APPEND in den Posteingang von geops@,
#                                 outlook.com und Gmail (nicht privat@: Quota); im Standardlauf
#                                 enthalten
#   tests/testdaten.py --anhang-teile KONTO ORDNER
#                                 #356, nur lesend, unabhängig von Camel: die Anhänge der Anhang-Mail
#                                 direkt vom Server (IMAP EXAMINE + UID FETCH BODY.PEEK[] bzw. Graph
#                                 GET …/attachments) nach ORDNER schreiben; je Anhang eine Zeile
#                                 „TEIL <sha256> <Bytes> <Dateiname>“, für Text zusätzlich „TEIL-TEXT …“
#                                 mit Zeilenenden CRLF → LF (so dekodiert Camel text/*)
#   tests/testdaten.py --kombiniert [LOG]   (#415: ohne Konten aus REVOLUTION_SKIP_ACCOUNTS bei REVOLUTION_TEST_HOOKS=1)
#                                 #351, nur lesend, unabhängig von Camel, jedes Konto aus GOA: je
#                                 Posteingang sichtbare und ungelesene Nachrichten (IMAP EXAMINE +
#                                 UID SEARCH UNDELETED ohne Junk, Graph GET); Zeilen „KOMBINIERT <UID>
#                                 <Adresse> <sichtbar> <ungelesen>“ und „SUMME …“. Mit LOG Vergleich mit
#                                 dem letzten „Favoriten: Anzahl inbox <UID> …“ je Konto, der Summe der
#                                 Liste und „Ungelesen: revolution-favoriten inbox <N>“; Exit 1 bei
#                                 Abweichung
#   tests/testdaten.py --konversation
#                                 #357: Konversation aus 5 Mails (Message-ID, In-Reply-To, References):
#                                 3 eingehende per APPEND in den Posteingang, 2 eigene Antworten per
#                                 APPEND in den Gesendet-Ordner, in geops@ (IMAP) und outlook.com
#                                 (microsoft365); Gesendet wie die App (#350: Ordnertyp vom Server,
#                                 sonst ESource „sent-folder“, sonst Ersteinrichtung des Stores).
#                                 Idempotent (Erkennung über Betreff und Absendedatum); letzte Zeilen
#                                 „KONVERSATION <Adresse> <UID>/<Ordner> <Anzahl>“ je Ordner
#   tests/testdaten.py --konversation-flags KONTO
#                                 #357, nur lesend, unabhängig von Camel: je Mail der Konversation
#                                 „KFLAGS <Nr> <gelesen 0|1> <markiert 0|1> <Ordner>“ (IMAP EXAMINE + UID
#                                 SEARCH HEADER Message-ID + UID FETCH FLAGS in INBOX und Sent bzw. Graph
#                                 GET nach internetMessageId; markiert = importance high wie --flags)
#   tests/testdaten.py --sortierung KONTO basis|neu|papierkorb
#                                 #418, schreibend nur in Testkonten (IMAP, nicht privat@): basis legt
#                                 „Pruef418 Faden“ (ungelesen, #419) mit festem Datum in der Vergangenheit
#                                 und „Pruef418 Oben“ (Datum jetzt) in den Posteingang (idempotent); neu legt eine
#                                 Antwort auf den Faden (Datum jetzt) und eine neue Konversation
#                                 „Pruef418 Neu“ (eine Sekunde davor) ab;
#                                 papierkorb verschiebt alle Pruef418-Mails per UID MOVE in den
#                                 Papierkorb (nicht endgültig gelöscht). Zeilen „ORDNER <UID>/INBOX“
#                                 und „SORTIERUNG <Rolle> <IMAP-UID> <Datum>“
#   tests/testdaten.py --suche    #365: Such-Mail (Wort nur im Text) per APPEND in geops@ (Posteingang und
#                                 Revolution-Test) und outlook.com (Posteingang), idempotent; Zeilen
#                                 „SUCHE <Adresse> <UID>/<Ordner>“
#   tests/testdaten.py --suche-zahlen KONTO ORDNER
#                                 #365, nur lesend, unabhängig von Camel: Treffer des Suchworts (IMAP UID SEARCH
#                                 OR SUBJECT/FROM/TO/CC/BODY bzw. Graph $search) und der Filter (UNSEEN, FLAGGED,
#                                 Anhang aus BODYSTRUCTURE bzw. Graph isRead/importance/hasAttachments), ohne
#                                 Gelöschte und Junk; ORDNER * = alle Ordner (IMAP LIST bzw. Graph /me/messages, nur Text)
#   tests/testdaten.py --antwort  #359, schreibend nur in geops@ (IMAP): Testmail für Antworten/Allen
#                                 antworten per APPEND in den Posteingang (kein Versand), idempotent über
#                                 die Message-ID; mehrere Empfänger, „RE:“ im Betreff, References. Absender
#                                 und fremde Empfänger als Plus-Adressen der Testkonten (keine Identität in
#                                 GOA, sonst entfernt sie Allen antworten wie Evolution); R2: dazu Konversation
#                                 mit eigener Antwort im Gesendet-Ordner und eine eigene Mail im Posteingang
#                                 (Antwort auf eigene Mail); Zeile „ANTWORT <UID>/INBOX <Betreff>“
#   tests/testdaten.py --antwort-kopf
#                                 #359, nur lesend (IMAP EXAMINE + UID FETCH): Kopfzeilen der Testmail vom
#                                 Server, je Zeile „KOPF <Name>: <Wert>“ (Vergleich mit der Antwort)
#   tests/testdaten.py --benachrichtigung KONTO neu NAME[,NAME…]
#                                 #366, schreibend nur in Testkonten (IMAP): je NAME eine Testmail per APPEND
#                                 in den Posteingang, Absender „Revolution Test“, NAME „gelesen…“ mit \Seen;
#                                 Zeile „NEU <UIDVALIDITY> <UID> <Betreff>“ (APPENDUID)
#   tests/testdaten.py --benachrichtigung KONTO gelesen UID
#                                 #366: eine eigene Testmail (Betreff geprüft) per UID STORE +FLAGS (\Seen) gelesen
#   tests/testdaten.py --benachrichtigung KONTO weg DATEI
#                                 #366: genau die UIDs aus den NEU-Zeilen in DATEI löschen (gleiche UIDVALIDITY,
#                                 Betreff geprüft): UID STORE +FLAGS (\Deleted) und UID EXPUNGE je UID, Zeilen
#                                 „BEFEHL …“
#   tests/testdaten.py --kontakt [anlegen|weg]
#                                 #361: Testkontakt „Revolution-Test Kontakt 361“ (zwei Adressen) im Google-Adressbuch (CardDAV
#                                 über EDS) von KONTO_GOOGLE, nie in Persönlich/lokal; ohne Aktion nur
#                                 lesend; anlegen, falls er fehlt; weg löscht genau die Kontakte mit diesem Namen
#                                 (UID-weise); Zeilen „ADRESSBUCH <UID> <Name>“, „KONTAKT <UID> <Name> <Adresse>“
#   tests/testdaten.py --adressbuecher
#                                 #368, nur lesend, eigener Prozess (Python-GI EBook.BookClient, dieselbe Schicht wie
#                                 GNOME Contacts): je aktiviertem Adressbuch „ADRESSBUCH <UID> <Konto> / <Name>“ mit
#                                 „nurlesend 0|1 initial 0|1 kontakte N listen L“ (N ohne Kontaktlisten, Abfrage wie die App
#                                 (contains "x-evolution-any-field" ""), nur bei do-initial-query, sonst „-“)
#   tests/testdaten.py --kontakt-live anlegen|aendern|loeschen|pruefen|refresh BUCH DATEI
#                                 #368, schreibend nur in den freigegebenen Büchern (KONTAKT_SCHREIBBAR: Google-CardDAV
#                                 Gmail test account, microsoft365 „Kontakte“ outlook.com, „Persönlich“): anlegen legt
#                                 „Live Revolution-Test 368 <Zeit>“ (Nachname „Revolution-Test 368 <Zeit>“) an und schreibt
#                                 „KONTAKT <BUCH> <UID> <Name>“ nach DATEI; aendern/loeschen nur genau diese UID aus DATEI und
#                                 nur mit Nachname „Revolution-Test…“ (modify_contact_sync, remove_contact_by_uid_sync);
#                                 pruefen (nur lesend) „VORHANDEN 0|1 <UID> <Name>“; refresh e_book_client_refresh_sync.
#                                 Zeilen „ANGELEGT|GEAENDERT|GELOESCHT <UID> <Name>“, „SCHREIBVORGANG <BUCH>“ je Schreiben
#   tests/testdaten.py --kontakt-graph anlegen|loeschen DATEI
#                                 #368, Gegenprobe am Server ohne EDS (Graph, nur KONTO_M365 /me/contacts): anlegen
#                                 POST mit Nachname „Revolution-Test 368 Graph <Zeit>“, ID nach DATEI („GRAPH <ID> <Name>“);
#                                 loeschen DELETE genau dieser ID (Nachname vorher per GET geprüft)
#   tests/testdaten.py --entwurf KONTO BETREFF [weg MESSAGE-ID …]
#                                 #360, unabhängig von Camel: Entwürfe mit genau diesem Betreff (je Lauf eindeutig,
#                                 Präfix „[Revolution-Test] Entwurf 360“) im Entwürfe-Ordner (IMAP: LIST mit \\Drafts,
#                                 EXAMINE + UID SEARCH + UID FETCH FLAGS; Graph GET drafts und deleteditems); Zeilen
#                                 „ENTWURFORDNER <Name>“, „ENTWURF <UID|ID> <gelöscht 0|1> <Betreff>“ und
#                                 „ENTWUERFE <sichtbar> <gelöscht>“. weg (nur Testkonten): vorher genau die Entwürfe mit
#                                 diesen Message-IDs (aus dem App-Log) löschen: IMAP UID STORE +FLAGS \\Deleted und
#                                 UID EXPUNGE nur dieser UIDs, Graph permanentDelete nur dieser IDs (drafts, deleteditems)
#   tests/testdaten.py --ort KONTO aktion|konversation [anlegen|zurueck|aufraeumen]
#                                 #363: Ordner am Server mit der Aktions-Mail bzw. den eingehenden Mails der
#                                 Konversation (#357), per Message-ID (IMAP LIST + EXAMINE + UID SEARCH bzw.
#                                 Graph GET); Zeilen „ORT <Nr> <Ordner> <gelesen 0|1>“, „HEIM <Ordner>“. anlegen:
#                                 Ordner Revolution-Aktionen und …-Ziel daneben, die Aktions-Mail im ersten
#                                 (APPEND bzw. Graph); zurueck: per UID MOVE bzw. Graph move zurück nach
#                                 Revolution-Aktionen bzw. INBOX; aufraeumen: Aktions-Mail endgültig löschen
#                                 (nur ihre UIDs bzw. IDs) und die beiden Ordner entfernen (nur Testkonten)
#   tests/testdaten.py --live-flags KONTO
#                                 #351, nur lesend: Flags der Live-Mails (--server KONTO neu INBOX) im
#                                 Posteingang (IMAP EXAMINE + UID SEARCH SUBJECT + UID FETCH FLAGS);
#                                 Zeilen „FLAGS <gelesen 0|1> <markiert 0|1> <UID>“
#   tests/testdaten.py --zustellung KONTO MESSAGE-ID
#                                 #362, nur lesend, unabhängig von Camel: wie oft die Nachricht mit dieser
#                                 Message-ID in Posteingang, Spam und Gesendet von KONTO liegt (IMAP EXAMINE +
#                                 UID SEARCH HEADER Message-ID; Ordner per SPECIAL-USE bzw. Name; Graph GET
#                                 nach internetMessageId, Ordner inbox/junkemail/sentitems); Zeilen
#                                 „ZUSTELLUNG <Adresse> eingang|spam|gesendet <N>“
#   tests/testdaten.py --aufraeumen KONTO MESSAGE-ID …
#                                 #362, schreibend nur in Testkonten: die vom Test gesendeten Mails (Betreff
#                                 mit [Revolution-Test], Message-ID wie angegeben) endgültig löschen, in
#                                 Posteingang, Spam und Gesendet (IMAP UID STORE \Deleted + UID EXPUNGE, Gmail
#                                 erst UID MOVE in den Papierkorb; Graph POST …/permanentDelete); Zeilen
#                                 „AUFGERAEUMT <Adresse> <Ordner> <N>“
#   tests/testdaten.py --konten   #364, nur lesend, ohne Verbindung: je Mail-Konto aus GOA „KONTO <UID> <Adresse>“
#                                 (für REVOLUTION_SKIP_ACCOUNTS: alle außer dem Testkonto eines Laufs)
#   tests/testdaten.py --ordnertest KONTO
#                                 #364, nur lesend, unabhängig von Camel: Ordner, deren Pfad einen Teil
#                                 „Revolution-Ordnertest…“ hat, direkt vom Server (IMAP LIST "" * auch
#                                 \Noselect, Camel-Namen mit „/“; Graph GET mailFolders/childFolders mit
#                                 Anzeigenamen); Zeilen „ORDNERTEST <Name>“, zuletzt „ORDNERTEST-ANZAHL <N>“
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import base64
import ctypes
import datetime
import hashlib
import imaplib
import io
import json
import re
import ssl
import email.header
import email.policy
import email.utils
import os
import urllib.error
import urllib.parse
import urllib.request
import struct
import sys
import threading
import time
import zipfile
import zlib
from email.message import EmailMessage

import gi
gi.require_version('Camel', '1.2')
gi.require_version('EDataServer', '1.2')
gi.require_version('Goa', '1.0')
from gi.repository import GLib, Gio, Camel, EDataServer, Goa

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testkonten import (KONTO_IMAP_1, KONTO_IMAP_2, KONTO_M365, KONTO_GOOGLE,  # noqa: E402
                        NUR_LESEND_1, NUR_LESEND_2, NUR_LESEND_3, HILFS_JANA, HILFS_LENA, HILFS_TOM,
                        HILFS_KONTAKT361, MSGID_DOMAIN)  # noqa: E402

PREFIX = '[Revolution-Test]'
TESTKONTEN = (KONTO_IMAP_1, KONTO_IMAP_2,
              KONTO_M365, KONTO_GOOGLE)
NUR_LESEND = (NUR_LESEND_1, NUR_LESEND_2, NUR_LESEND_3)
ABSENDER = (KONTO_IMAP_1, KONTO_IMAP_2)
ORDNER = 'Revolution-Test'
LAST_ORDNER = 'Revolution-Last'
LAST_KONTO = KONTO_IMAP_1
UNTERORDNER = ORDNER + '/Archiv-Test'
REMOTE_BILD = 'https://upload.wikimedia.org/wikipedia/commons/a/a9/Example.jpg'
TIMEOUT = 90
WARTEN = 600
# #439: Camel-Cache im Ordner des Profils (USER_DIR aus tests/abbruch.sh bzw. BUILD/config.h), nie im
# Cache der installierten App (~/.cache/revolution); Standard Entwicklungsvariante.
CACHE = os.path.join(GLib.get_user_cache_dir(), os.environ.get('USER_DIR') or 'revolution-devel', 'testdaten')


def nur_testkonten(adressen):
    """Harte Prüfung: jede Adresse, an die gesendet oder in die geschrieben wird."""
    for a in adressen:
        if a.lower() in NUR_LESEND or a.lower() not in TESTKONTEN:
            sys.exit('ABBRUCH: %s ist kein Testkonto' % a)


def cancellable():
    c = Gio.Cancellable()
    t = threading.Timer(TIMEOUT, c.cancel)
    t.daemon = True
    t.start()
    return c


# ---------------------------------------------------------------- Testdaten

def png_bytes():
    w = h = 16
    raw = b''.join(b'\x00' + b'\x35\x84\xe4' * w for _ in range(h))

    def chunk(typ, data):
        return (struct.pack('>I', len(data)) + typ + data
                + struct.pack('>I', zlib.crc32(typ + data) & 0xffffffff))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def pdf_bytes():
    text = b'BT /F1 18 Tf 72 720 Td (Revolution-Test) Tj ET'
    objs = [b'<< /Type /Catalog /Pages 2 0 R >>',
            b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
            b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] '
            b'/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
            b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
            b'<< /Length %d >>\nstream\n%s\nendstream' % (len(text), text)]
    out = b'%PDF-1.4\n'
    offsets = []
    for i, o in enumerate(objs, 1):
        offsets.append(len(out))
        out += b'%d 0 obj\n%s\nendobj\n' % (i, o)
    xref = len(out)
    out += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objs) + 1)
    out += b''.join(b'%010d 00000 n \n' % off for off in offsets)
    out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objs) + 1, xref)
    return out


def ics_text():
    att = ''.join('ATTENDEE;CN=%s;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:%s\r\n'
                  % (a, a) for a in TESTKONTEN if a != KONTO_IMAP_1)
    return ('BEGIN:VCALENDAR\r\nPRODID:-//Revolution//testdaten//DE\r\nVERSION:2.0\r\n'
            'METHOD:REQUEST\r\nBEGIN:VEVENT\r\nUID:revolution-test-einladung@%s\r\n'
            'DTSTAMP:20261023T080000Z\r\nDTSTART:20261105T090000Z\r\nDTEND:20261105T100000Z\r\n'
            'SUMMARY:%s Abstimmung Testdaten\r\nLOCATION:Online\r\nSEQUENCE:0\r\n'
            'STATUS:CONFIRMED\r\nORGANIZER;CN=%s:mailto:%s\r\n'
            '%sEND:VEVENT\r\nEND:VCALENDAR\r\n' % (MSGID_DOMAIN, PREFIX, KONTO_IMAP_1, KONTO_IMAP_1, att))


def msgid(name):
    return '<revolution-test-%s@%s>' % (name, MSGID_DOMAIN)


# Jede Mail: Schlüssel, Absender, Betreff, Bauanweisung, gelesen?
# Erkennung „schon vorhanden“ über (Betreff, Absender).
def baue(key):
    m = EmailMessage(policy=email.policy.SMTP)
    if key == 'text':
        m.set_content('Einfache Text-Mail. Bleibt ungelesen.\n')
    elif key == 'html':
        m.set_content('HTML-Mail mit Remote-Bild: %s\n' % REMOTE_BILD)
        m.add_alternative('<html><body><p>HTML-Mail mit <b>Remote-Bild</b>:</p>'
                          '<p><img src="%s" alt="Beispielbild" width="172" height="178"></p>'
                          '</body></html>\n' % REMOTE_BILD, subtype='html')
    elif key == 'anhang':
        m.set_content('Mail mit zwei Anhängen (PNG, PDF).\n')
        m.add_attachment(png_bytes(), maintype='image', subtype='png', filename='revolution-test.png')
        m.add_attachment(pdf_bytes(), maintype='application', subtype='pdf', filename='revolution-test.pdf')
    elif key.startswith('thread'):
        m.set_content('Thread-Mail %s von 3.\n' % key[-1])
    elif key == 'einladung':
        m.set_content('Einladung: Abstimmung Testdaten am 05.11.2026, 10:00 Uhr.\n')
        m.add_alternative(ics_text(), subtype='calendar', params={'method': 'REQUEST'})
    return m


MAILS = [
    ('text', KONTO_IMAP_1, PREFIX + ' Einfache Text-Mail', False),
    ('html', KONTO_IMAP_2, PREFIX + ' HTML mit Remote-Bild', True),
    ('anhang', KONTO_IMAP_1, PREFIX + ' Anhänge PNG und PDF', True),
    ('thread1', KONTO_IMAP_1, PREFIX + ' Projektplanung', True),
    ('thread2', KONTO_IMAP_2, PREFIX + ' Re: Projektplanung', True),
    ('thread3', KONTO_IMAP_1, PREFIX + ' Re: Projektplanung', True),
    ('einladung', KONTO_IMAP_1, PREFIX + ' Einladung: Abstimmung Testdaten', True),
]
KOPIEN = {'anhang': UNTERORDNER, 'einladung': ORDNER}

# #354: Mails für die Anzeige, per APPEND (kein Versand) in den Posteingang jedes Testkontos.
# Absender als Kopfzeile ein Testkonto mit Anzeigenamen. Erkennung wie MAILS über (Betreff, Absender).
ANZEIGE_ABSENDER = KONTO_IMAP_1
ANZEIGE = [
    ('anzeige-html', PREFIX + ' Anzeige HTML: Jahresabrechnung'),
    ('anzeige-latin1', PREFIX + ' Anzeige Text ISO-8859-1: Grüße aus Köln'),
    ('anzeige-cid', PREFIX + ' Anzeige Inline-Bild (cid)'),
    ('anzeige-remote', PREFIX + ' Anzeige Remote-Inhalte (Tracking-Pixel)'),
    ('anzeige-dunkel', PREFIX + ' Anzeige HTML mit Dunkel-Fassung'),
    # #355: zweite Mail desselben Absenders und eine von einem anderen Absender, je mit Remote-Bild
    ('anzeige-remote2', PREFIX + ' Anzeige Remote zweite Mail'),
    ('anzeige-remote-fremd', PREFIX + ' Anzeige Remote anderer Absender'),
]
# #355: abweichender Absender je Schlüssel (sonst ANZEIGE_ABSENDER)
ANZEIGE_ANDERE = {'anzeige-remote-fremd': KONTO_IMAP_2}
# #411: nicht mehr in privat@ (Quota fast voll); kein Test braucht die Anzeige-Mails dort.
ANZEIGE_KONTEN = (KONTO_IMAP_2, KONTO_M365, KONTO_GOOGLE)
REMOTE_HOST = 'https://upload.wikimedia.org'


def anzeige_html():
    zeilen = ''.join('<tr><td style="padding:6px 0">%s</td><td style="padding:6px 0;text-align:right">%s</td></tr>'
                     % z for z in [('Abrechnungszeitraum', '01.01. – 31.12.2026'), ('Verbrauch Strom', '2.418 kWh'),
                                   ('Neuer Abschlag ab Januar', '72,00 € monatlich'),
                                   ('<b>Ihr Guthaben</b>', '<b style="color:#0b7fab">84,20 €</b>')])
    absatz = ('<p>Das Guthaben überweisen wir in den nächsten Tagen auf Ihr Konto. Den neuen Abschlag '
              'haben wir an Ihren Verbrauch angepasst. Fragen beantworten wir gern unter '
              '<a href="mailto:%s">%s</a>.</p>' % (KONTO_IMAP_1, KONTO_IMAP_1))
    return ('<!DOCTYPE html><html><head><meta charset="utf-8"><style>'
            'body{font-family:sans-serif;color:#1d1d1d;margin:0}'
            '.rahmen{max-width:640px;margin:0 auto;padding:32px 40px}'
            '.kopf{display:flex;justify-content:space-between;border-bottom:2px solid #0b7fab;padding-bottom:12px}'
            '.marke{color:#0b7fab;font-size:22px;font-weight:bold}'
            '.box{background:#eef5f9;border-radius:10px;padding:14px 22px;margin:20px 0}'
            '.knopf{display:inline-block;background:#0b7fab;color:#fff;padding:12px 24px;border-radius:4px;'
            'text-decoration:none;font-weight:bold}'
            '@media (prefers-color-scheme: dark){.box{background:#1d3340}}'
            '</style></head><body><div class="rahmen">'
            '<div class="kopf"><span class="marke">Stadtwerke Lindenau</span>'
            '<span style="color:#666;font-size:13px">Kundennummer 104 227 1</span></div>'
            '<h1>Ihre Jahresabrechnung 2026 ist da</h1>'
            '<p>Sehr geehrte Frau Vogt,</p><p>vielen Dank, dass Sie Ihre Energie von uns beziehen. Ihre '
            '<b>Jahresabrechnung für 2026</b> steht ab sofort bereit. Sie haben weniger verbraucht als im '
            'Vorjahr und erhalten ein <b>Guthaben</b>. Umlaute und Zeichen: äöü ÄÖÜ ß € „Anführung“ – Strich.</p>'
            '<table class="box" width="100%%">%s</table>%s'
            '<p><a class="knopf" href="https://www.gnome.org/">Zur Rechnung im Kundenportal</a></p>'
            '%s<p>Freundliche Grüße<br>Ihr Kundenservice der Stadtwerke Lindenau</p>'
            '<hr><p style="color:#666;font-size:12px">Testmail für Revolution #354 (HTML, UTF-8, ohne '
            'Remote-Inhalte). Ende der Nachricht.</p></div></body></html>\n'
            % (zeilen, absatz * 3, absatz * 3))


def anzeige_dunkel_html():
    """#354 Nacharbeit: Mail mit vollständiger eigener Dunkel-Fassung (meta color-scheme „light dark“,
    Medienabfrage setzt Hintergrund und Schrift) und einem Link mit ping-Attribut (Hyperlink-Auditing)."""
    return ('<!DOCTYPE html><html><head><meta charset="utf-8">'
            '<meta name="color-scheme" content="light dark"><meta name="supported-color-schemes" content="light dark">'
            '<style>body{font-family:sans-serif;margin:0;background:#ffffff;color:#1d1d1d}'
            '.rahmen{max-width:640px;margin:0 auto;padding:32px 40px}'
            '.marke{color:#0b7fab;font-size:22px;font-weight:bold}'
            '.box{background:#eef5f9;border-radius:10px;padding:14px 22px;margin:20px 0}'
            '.knopf{display:inline-block;background:#0b7fab;color:#fff;padding:12px 24px;border-radius:4px;'
            'text-decoration:none;font-weight:bold}'
            '.hinweis{color:#666;font-size:12px}'
            '@media (prefers-color-scheme: dark){body{background:#10222c;color:#e8eef2}.marke{color:#5cc8f0}'
            '.box{background:#1d3340;color:#e8eef2}.hinweis{color:#9fb3bf}}'
            '</style></head><body><div class="rahmen">'
            '<div class="marke">Stadtwerke Lindenau</div>'
            '<h1>Abschlag ab Januar</h1>'
            '<p>Diese Mail hat eine eigene Dunkel-Fassung (Hintergrund #10222c). Helle Fassung: weiß.</p>'
            '<div class="box">Neuer Abschlag ab Januar: <b>72,00 € monatlich</b></div>'
            '<p><a class="knopf" href="https://www.gnome.org/" ping="%s/revolution-ping">Zum Kundenportal</a></p>'
            '<p class="hinweis">Testmail für Revolution #354 (eigene Dunkel-Fassung, Link mit ping).</p>'
            '</div></body></html>\n' % REMOTE_HOST)


def anzeige_nachricht(key, betreff):
    m = EmailMessage(policy=email.policy.SMTP)
    if key == 'anzeige-html':
        m.set_content('Ihre Jahresabrechnung 2026 ist da (Textfassung).\n')
        m.add_alternative(anzeige_html(), subtype='html')
    elif key == 'anzeige-latin1':
        m.set_content('Hallo Arnd,\n\nviele Grüße aus Köln! Umlaute in ISO-8859-1: äöü ÄÖÜ ß, '
                      'dazu »Zitat« und ½ Liter.\n\nLink: https://www.gnome.org/ und Adresse '
                      '%s\n\nEinrückung:\n    vier Leerzeichen\n\tein Tab\n\n'
                      '> Zitierte Zeile aus der vorigen Mail\n> mit zweiter Zeile\n\n'
                      'Tschüß\nRevolution #354\n' % KONTO_IMAP_1, charset='iso-8859-1')
    elif key == 'anzeige-cid':
        m.set_content('Inline-Bild nur in der HTML-Fassung.\n')
        m.add_alternative('<html><body><p>Inline-Bild aus der Mail (cid:), blaues Quadrat 16×16 '
                          'auf 64×64 skaliert:</p><p><img src="cid:revolution-bild@%s" '
                          'width="64" height="64" alt="Inline-Bild"></p><p>Ende.</p></body></html>\n' % MSGID_DOMAIN,
                          subtype='html')
        m.get_payload()[1].add_related(png_bytes(), maintype='image', subtype='png',
                                       cid='<revolution-bild@%s>' % MSGID_DOMAIN, filename='bild.png')
    elif key == 'anzeige-dunkel':
        m.set_content('Abschlag ab Januar (Textfassung).\n')
        m.add_alternative(anzeige_dunkel_html(), subtype='html')
    elif key == 'anzeige-remote':
        h = REMOTE_HOST
        m.set_content('Remote-Inhalte nur in der HTML-Fassung.\n')
        m.add_alternative(
            '<html><head><link rel="stylesheet" href="%s/revolution-test.css">'
            '<link rel="preconnect" href="%s"><link rel="dns-prefetch" href="%s">'
            '<style>@import url("%s/revolution-import.css"); .bg{background:url("%s/revolution-bg.png")}</style>'
            '</head><body><p>Remote-Inhalte (dürfen ohne Freigabe nicht laden, #354/#355):</p>'
            '<p class="bg"><img src="%s" alt="Remote-Bild" width="172" height="178"></p>'
            '<img src="%s/revolution-pixel.gif?id=354" width="1" height="1" alt="">'
            '<video src="%s/revolution.webm" width="10" height="10"></video>'
            '<iframe src="%s/revolution-frame.html" width="10" height="10"></iframe>'
            '<p>Ende.</p></body></html>\n' % (h, h, h, h, h, REMOTE_BILD, h, h, h), subtype='html')
    elif key in ('anzeige-remote2', 'anzeige-remote-fremd'):
        m.set_content('Remote-Bild nur in der HTML-Fassung (#355).\n')
        m.add_alternative('<html><body><p>Remote-Bild (%s, #355):</p><p><img src="%s?%s" alt="Remote-Bild" '
                          'width="172" height="178"></p><p>Ende.</p></body></html>\n'
                          % (key, REMOTE_BILD, key), subtype='html')
    absender = ANZEIGE_ANDERE.get(key, ANZEIGE_ABSENDER)
    m['Subject'] = betreff
    m['From'] = email.utils.formataddr(('Revolution Testdaten', absender))
    m['To'] = ANZEIGE_ABSENDER
    m['Cc'] = KONTO_IMAP_2
    m['Date'] = email.utils.formatdate(localtime=True)
    m['Message-ID'] = msgid(key)
    msg = Camel.MimeMessage.new()
    msg.construct_from_stream_sync(Camel.StreamMem.new_with_buffer(m.as_bytes()), None)
    return msg


def anzeige(konten, dry):
    """#354: fehlende Anzeige-Mails per APPEND in den Posteingang (idempotent, als gelesen)."""
    for a, k in konten.items():
        if a not in ANZEIGE_KONTEN:
            continue
        nur_testkonten([a])
        k.inbox.refresh_info_sync(cancellable())
        vorhanden = testmails(k.inbox)
        neu = 0
        for key, betreff in ANZEIGE:
            if (betreff, ANZEIGE_ANDERE.get(key, ANZEIGE_ABSENDER)) in vorhanden:
                continue
            print('  %s: %s per APPEND' % (a, betreff))
            neu += 1
            if dry:
                continue
            info = Camel.MessageInfo.new(None)
            info.set_flags(Camel.MessageFlags.SEEN, Camel.MessageFlags.SEEN)
            k.inbox.append_message_sync(anzeige_nachricht(key, betreff), info, cancellable())
        if neu and not dry:
            k.inbox.synchronize_sync(False, cancellable())
            k.inbox.refresh_info_sync(cancellable())


# #356: Mail mit Anhängen für die Anhangleiste, per APPEND (kein Versand) in den Posteingang der
# Testkonten außer privat@ (Quota fast voll). Inhalte selbst erzeugt und schadlos, fest (gleiche Bytes
# bei jedem Lauf). Erkennung wie ANZEIGE über (Betreff, Absender).
ANHANG_KONTEN = (KONTO_IMAP_2, KONTO_M365, KONTO_GOOGLE)
ANHANG_BETREFF = PREFIX + ' Anhänge PDF, Bild, ZIP'
ANHANG_ID = 'anhang-356'


def bild_png():
    """PNG 96×64 mit Farbverlauf (RGB), ein paar KB."""
    w, h = 96, 64
    raw = b''.join(b'\x00' + b''.join(bytes((40 + x * 2, 90 + y * 2, 200 - x)) for x in range(w)) for y in range(h))

    def chunk(typ, data):
        return (struct.pack('>I', len(data)) + typ + data
                + struct.pack('>I', zlib.crc32(typ + data) & 0xffffffff))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def zip_bytes():
    """ZIP mit zwei Textdateien, feste Zeitstempel (gleiche Bytes bei jedem Lauf)."""
    puffer = io.BytesIO()
    with zipfile.ZipFile(puffer, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, text in (('liesmich.txt', 'Revolution #356: Testarchiv, schadlos.\n'),
                           ('daten.csv', 'Nr;Name\n1;Lageplan\n2;Marktstand\n')):
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 23, 12, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, text.encode())
    return puffer.getvalue()


def anhang_nachricht():
    m = EmailMessage(policy=email.policy.SMTP)
    m.set_content('Mail mit Anhängen für Revolution #356: PDF, PNG, ZIP, eine Textdatei mit „../“ im '
                  'Dateinamen (Bereinigung) und eine angehängte Nachricht.\n')
    m.add_attachment(pdf_bytes(), maintype='application', subtype='pdf', filename='Lageplan Übersicht.pdf')
    m.add_attachment(bild_png(), maintype='image', subtype='png', filename='Marktstand.png')
    m.add_attachment(zip_bytes(), maintype='application', subtype='zip', filename='Unterlagen.zip')
    m.add_attachment('Dateiname mit Pfadanteil, darf nur als revolution-pfad.txt landen.\n',
                     filename='../../revolution-pfad.txt')
    innen = EmailMessage(policy=email.policy.SMTP)
    innen.set_content('Angehängte Nachricht (message/rfc822) für #356.\n')
    innen['Subject'] = 'Weitergeleitet: Termin Hoffest'
    innen['From'] = email.utils.formataddr(('Revolution Testdaten', ANZEIGE_ABSENDER))
    innen['To'] = ANZEIGE_ABSENDER
    innen['Date'] = 'Fri, 23 Oct 2026 12:00:00 +0200'
    innen['Message-ID'] = msgid('anhang-356-innen')
    m.add_attachment(innen)
    m['Subject'] = ANHANG_BETREFF
    m['From'] = email.utils.formataddr(('Revolution Testdaten', ANZEIGE_ABSENDER))
    m['To'] = ANZEIGE_ABSENDER
    m['Date'] = email.utils.formatdate(localtime=True)
    m['Message-ID'] = msgid(ANHANG_ID)
    msg = Camel.MimeMessage.new()
    msg.construct_from_stream_sync(Camel.StreamMem.new_with_buffer(m.as_bytes()), None)
    return msg


def anhang(konten, dry):
    """#356: Anhang-Mail per APPEND in den Posteingang (idempotent, als gelesen), nicht privat@."""
    for a in ANHANG_KONTEN:
        k = konten[a]
        nur_testkonten([a])
        k.inbox.refresh_info_sync(cancellable())
        if (ANHANG_BETREFF, ANZEIGE_ABSENDER) in testmails(k.inbox):
            print('  %s: %s vorhanden' % (a, ANHANG_BETREFF))
            continue
        print('  %s: %s per APPEND' % (a, ANHANG_BETREFF))
        if dry:
            continue
        info = Camel.MessageInfo.new(None)
        info.set_flags(Camel.MessageFlags.SEEN, Camel.MessageFlags.SEEN)
        k.inbox.append_message_sync(anhang_nachricht(), info, cancellable())
        k.inbox.synchronize_sync(False, cancellable())
        k.inbox.refresh_info_sync(cancellable())


def struktur(m):
    """#360: Aufbau einer Nachricht als Text, z. B. multipart/mixed[text/plain,message/rfc822[text/plain]]."""
    if m.is_multipart() or m.get_content_type() == 'message/rfc822':
        return '%s[%s]' % (m.get_content_type(), ','.join(struktur(t) for t in m.get_payload()))
    return m.get_content_type()


def anhang_teile(k, ziel):
    """#356: nur lesend, ohne Camel: Anhänge der Anhang-Mail vom Server nach ziel schreiben."""
    os.makedirs(ziel, exist_ok=True)
    teile = []
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        c.select('INBOX', readonly=True)
        uids = c.uid('SEARCH', None, 'HEADER', 'Message-ID', msgid(ANHANG_ID))[1][0].split()
        if not uids:
            sys.exit('ABBRUCH: Anhang-Mail fehlt in %s' % k.adresse)
        roh = c.uid('FETCH', uids[-1], '(BODY.PEEK[])')[1][0][1]
        c.logout()
        nachricht = email.message_from_bytes(roh, policy=email.policy.default)
        print('STRUKTUR %s' % struktur(nachricht))
        for teil in nachricht.iter_attachments():
            if teil.get_content_type() == 'message/rfc822':
                teile.append((teil.get_payload()[0]['Subject'] + '.eml', teil.get_payload()[0].as_bytes()))
            else:
                teile.append((teil.get_filename(), teil.get_payload(decode=True)))
    else:
        mails = graph(k, 'GET', "/me/messages?$filter=internetMessageId%20eq%20'"
                      + urllib.request.quote(msgid(ANHANG_ID)) + "'&$select=id")['value']
        if not mails:
            sys.exit('ABBRUCH: Anhang-Mail fehlt in %s' % k.adresse)
        for a in graph(k, 'GET', '/me/messages/%s/attachments' % mails[0]['id'])['value']:
            if 'contentBytes' in a:
                teile.append((a['name'], base64.b64decode(a['contentBytes'])))
            else:
                print('OHNE-BYTES %s %s' % (a['@odata.type'], a['name']))
    for name, daten in teile:
        sicher = name.replace('/', '_')
        with open(os.path.join(ziel, sicher), 'wb') as f:
            f.write(daten)
        print('TEIL %s %d %s' % (hashlib.sha256(daten).hexdigest(), len(daten), name))
        if name.endswith('.txt'):
            # Camel dekodiert text/* mit CRLF → LF (camel-data-wrapper.c:326-335), Evolution ebenso.
            lf = daten.replace(b'\r\n', b'\n')
            print('TEIL-TEXT %s %d %s (CRLF→LF)' % (hashlib.sha256(lf).hexdigest(), len(lf), name))
    return 0


# #357: Thread mit 5 Mails. Feste Daten und Message-IDs (gleiche Bytes bei jedem Lauf); Antworten mit
# In-Reply-To auf die vorige und References auf alle vorigen (RFC 5322 3.6.4), wie Camel sie für Threads
# liest (camel_folder_thread_messages_new, camel-folder-thread.c:540-610).
KONVERSATION_KONTEN = (KONTO_IMAP_2, KONTO_M365)
KONVERSATION_BETREFF = PREFIX + ' Konversation Marktstand'
KONVERSATION_PARTNER = ('Jana Albrecht', ANZEIGE_ABSENDER)
# Nr. 3 als HTML mit entferntem Bild und Anhang: Sperre (#355) und Anhänge (#356) je Nachricht der
# Konversation.
KONVERSATION = [
    # (Nr, eigene Antwort?, gelesen?, Text)
    (1, False, True, 'Hallo, steht der Marktstand am Samstag noch? Ich bringe die Kisten mit.'),
    (2, True, True, 'Ja, Samstag ab 8 Uhr. Die Kisten kommen links neben den Tisch.'),
    (3, False, True, 'Prima. Brauchen wir noch ein Verlängerungskabel für die Waage?'),
    (4, True, True, 'Ja, bitte eins mit 10 m mitbringen, die Steckdose ist am Brunnen.'),
    (5, False, False, 'Mache ich. Bis Samstag!'),
]


# Feste Daten in der Vergangenheit (PM 24.10.2026: nie in der Zukunft, sonst stehen sie in anderen Tests
# oben); fest, damit die Erkennung über das Absendedatum idempotent bleibt.
def konversation_datum(nr):
    return datetime.datetime(2026, 10, 23, 8, 0, tzinfo=datetime.timezone(datetime.timedelta(hours=2))) \
        + datetime.timedelta(minutes=17 * (nr - 1))


def konversation_nachricht(nr, eigen, text, konto):
    m = EmailMessage(policy=email.policy.SMTP)
    m.set_content(text + '\n')
    if nr == 3:
        m.add_alternative('<html><body><p style="color:#1a5fb4">%s</p><p><img src="%s" alt="Waage" '
                          'width="172" height="178"></p><p>Der Plan hängt an.</p></body></html>\n'
                          % (text, REMOTE_BILD), subtype='html')
        m.add_attachment(bild_png(), maintype='image', subtype='png', filename='Marktstand-Plan.png')
    ich = email.utils.formataddr(('Revolution Test', konto))
    partner = email.utils.formataddr(KONVERSATION_PARTNER)
    m['Subject'] = KONVERSATION_BETREFF if nr == 1 else 'Re: ' + KONVERSATION_BETREFF
    m['From'] = ich if eigen else partner
    m['To'] = partner if eigen else ich
    m['Date'] = email.utils.format_datetime(konversation_datum(nr))
    m['Message-ID'] = msgid('konversation-357-%d' % nr)
    if nr > 1:
        m['In-Reply-To'] = msgid('konversation-357-%d' % (nr - 1))
        m['References'] = ' '.join(msgid('konversation-357-%d' % i) for i in range(1, nr))
    msg = Camel.MimeMessage.new()
    msg.construct_from_stream_sync(Camel.StreamMem.new_with_buffer(m.as_bytes()), None)
    return msg


def gesendet_holen(k, registry):
    """Gesendet-Ordner wie die App (#350, src/mail/mail-folder.c revolution_mail_folder_mark_special)."""
    for name, flags in ordnerbaum(k.store):
        if flags & Camel.FOLDER_TYPE_MASK == int(Camel.FolderInfoFlags.TYPE_SENT):
            return name, 'Ordnertyp'
    uid = k.account.get_uid()
    ident = registry.ref_source(k.account.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_identity_uid())
    uri = ident.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_SUBMISSION).get_sent_folder() if ident else None
    url = Camel.URL.new(uri) if uri else None
    if url and url.protocol == 'folder' and url.path and \
            ('%s@%s' % (url.user, url.host) if url.user else url.host) == uid:
        return Camel.URL.decode_path(url.path[1:]), 'ESource'
    ok, setup = k.store.initial_setup_sync(cancellable())
    name = setup.get('Submission:Mail Submission:sent-folder:f') if ok and setup else None
    if not name:
        sys.exit('ABBRUCH: kein Gesendet-Ordner in %s' % k.adresse)
    return name, 'Ersteinrichtung'


def konversation(konten, registry, dry):
    """#357: fehlende Mails der Konversation per APPEND (idempotent)."""
    for a in KONVERSATION_KONTEN:
        k = konten[a]
        nur_testkonten([a])
        sent_name, quelle = gesendet_holen(k, registry)
        sent = k.store.get_folder_sync(sent_name, 0, cancellable())
        print('  %s: Gesendet %s (%s)' % (a, sent_name, quelle))
        soll = {int(konversation_datum(nr).timestamp()) for nr, _, _, _ in KONVERSATION}
        for folder in (k.inbox, sent):
            folder.refresh_info_sync(cancellable())
            vorhanden = set()
            veraltet = 0
            for uid in folder.get_uids():
                info = folder.get_message_info(uid)
                if info and (info.get_subject() or '').endswith(KONVERSATION_BETREFF) \
                        and not info.get_flags() & Camel.MessageFlags.DELETED:
                    if info.get_date_sent() in soll:
                        vorhanden.add(info.get_date_sent())
                    else:
                        # Eigene Konversationsmails mit anderem Datum (frühere Fassung) ersetzen.
                        print('  %s: Konversation (veraltet, %s) löschen in %s' % (
                            a, datetime.datetime.fromtimestamp(info.get_date_sent()).isoformat(), folder.get_full_name()))
                        veraltet += 1
                        if not dry:
                            folder.set_message_flags(uid, Camel.MessageFlags.DELETED, Camel.MessageFlags.DELETED)
            if veraltet and not dry:
                folder.synchronize_sync(True, cancellable())
            neu = 0
            for nr, eigen, gelesen, text in KONVERSATION:
                if (folder is sent) != eigen:
                    continue
                if int(konversation_datum(nr).timestamp()) in vorhanden:
                    continue
                print('  %s: Konversation %d per APPEND in %s' % (a, nr, folder.get_full_name()))
                neu += 1
                if dry:
                    continue
                info = Camel.MessageInfo.new(None)
                if gelesen:
                    info.set_flags(Camel.MessageFlags.SEEN, Camel.MessageFlags.SEEN)
                folder.append_message_sync(konversation_nachricht(nr, eigen, text, a), info, cancellable())
            if neu and not dry:
                folder.synchronize_sync(False, cancellable())
                folder.refresh_info_sync(cancellable())
            anzahl = sum(1 for uid in folder.get_uids()
                         if (folder.get_message_info(uid).get_subject() or '').endswith(KONVERSATION_BETREFF))
            print('KONVERSATION %s %s/%s %d' % (a, k.account.get_uid(), folder.get_full_name(), anzahl))


def konversation_flags(k):
    """#357: nur lesend, ohne Camel: Flags der fünf Mails der Konversation vom Server."""
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        for ziel in ('INBOX', 'Sent'):
            name, camel = imap_ordner(c, ziel)
            c.select('"%s"' % name, readonly=True)
            for nr, _, _, _ in KONVERSATION:
                for uid in c.uid('SEARCH', None, 'HEADER', 'Message-ID', msgid('konversation-357-%d' % nr))[1][0].split():
                    flags = c.uid('FETCH', uid, '(FLAGS)')[1][0].decode()
                    if '\\Deleted' not in flags:
                        print('KFLAGS %d %d %d %s' % (nr, '\\Seen' in flags, '\\Flagged' in flags, camel))
        c.logout()
    else:
        for nr, _, _, _ in KONVERSATION:
            for m in graph(k, 'GET', "/me/messages?$filter=internetMessageId%20eq%20'"
                           + urllib.request.quote(msgid('konversation-357-%d' % nr))
                           + "'&$select=isRead,importance,parentFolderId")['value']:
                ordner = graph(k, 'GET', '/me/mailFolders/%s?$select=displayName' % m['parentFolderId'])['displayName']
                if ordner in ('Gelöschte Elemente', 'Deleted Items'):
                    continue
                print('KFLAGS %d %d %d %s' % (nr, m['isRead'], m['importance'] == 'high', ordner))
    return 0


# #418: Konversationsliste nach dem Server-Durchgang. basis: Faden mit festem Tag in der Vergangenheit
# (steht weit unten) und „Oben“ mit Datum jetzt (oberste Zeile im Cache). neu: Antwort auf den Faden mit
# Datum jetzt, neue Konversation eine Sekunde davor (nie in der Zukunft): nach dem Abgleich stehen
# Faden, Neu, Oben in dieser Folge oben.
SORTIERUNG_BETREFF = PREFIX + ' Pruef418 Faden'
SORTIERUNG_NEU = PREFIX + ' Pruef418 Neu'
SORTIERUNG_OBEN = PREFIX + ' Pruef418 Oben'


# #359: Testmail für Antworten und Allen antworten in geops@ (Posteingang). Eigene Adressen (Identitäten
# in GOA: geops@, privat@, KONTO_M365) entfernt Allen antworten wie Evolution
# (em-composer-utils.c:3669-3734, 3790-3810); die übrigen Adressen sind Plus-Adressen der Testkonten
# (Zustellung ins Testkonto, aber keine Identität). Festes Datum in der Vergangenheit.
ANTWORT_KONTO = KONTO_IMAP_2
ANTWORT_BETREFF = 'RE: ' + PREFIX + ' Standplan Samstag'
ANTWORT_MID = msgid('antwort-359-3')


def antwort_nachricht():
    m = EmailMessage(policy=email.policy.SMTP)
    m.set_content('Hallo zusammen,\n\nder Standplan für Samstag steht: Aufbau ab 8 Uhr, Stand 12.\n\nViele Grüße\nJana\n')
    m.add_alternative('<html><head><style>p { margin: 0 0 8px; }</style></head><body>'
                      '<p>Hallo zusammen,</p><p>der <b>Standplan</b> für Samstag steht: Aufbau ab 8 Uhr, '
                      '<span style="color:#1a5fb4">Stand 12</span>.</p><p>Viele Grüße<br>Jana</p>'
                      '</body></html>\n', subtype='html')
    m['Subject'] = ANTWORT_BETREFF
    m['From'] = email.utils.formataddr(('Jana Albrecht', HILFS_JANA))
    m['To'] = ', '.join([email.utils.formataddr(('Revolution Test', ANTWORT_KONTO)),
                         email.utils.formataddr(('Tom Becker', HILFS_TOM))])
    m['Cc'] = ', '.join([email.utils.formataddr(('Privat', KONTO_IMAP_1)),
                         email.utils.formataddr(('Lena Hoffmann', HILFS_LENA)),
                         KONTO_M365])
    m['Date'] = email.utils.format_datetime(
        datetime.datetime(2026, 10, 24, 9, 30, tzinfo=datetime.timezone(datetime.timedelta(hours=2))))
    m['Message-ID'] = ANTWORT_MID
    m['In-Reply-To'] = msgid('antwort-359-2')
    m['References'] = msgid('antwort-359-1') + ' ' + msgid('antwort-359-2')
    return m


# #359 R2: Antwort auf eigene Mail (Evolution em-composer-utils.c:5127-5131, 5149-5151). Konversation „Eigene
# Antwort Kisten“: Mail von Jana im Posteingang, eigene Antwort (neueste) im Gesendet-Ordner; dazu eine eigene
# Mail im Posteingang (Kopie an sich selbst, außerhalb von Gesendet; Betreff mit deutschem „AW:“ wie Outlook,
# em-utils.c:1661-1670). Feste Daten in der Vergangenheit.
EIGEN_BETREFF = PREFIX + ' Eigene Antwort Kisten'
EIGEN_EINGANG_BETREFF = 'AW: ' + PREFIX + ' Eigene Mail im Eingang'


def eigen_nachricht(nr):
    m = EmailMessage(policy=email.policy.SMTP)
    ich = email.utils.formataddr(('Revolution Test', ANTWORT_KONTO))
    jana = email.utils.formataddr(('Jana Albrecht', HILFS_JANA))
    lena = email.utils.formataddr(('Lena Hoffmann', HILFS_LENA))
    tom = email.utils.formataddr(('Tom Becker', HILFS_TOM))
    if nr == 1:
        m.set_content('Bringt jemand die Kisten mit?\n')
        m['Subject'], m['From'], m['To'], m['Cc'] = EIGEN_BETREFF, jana, ich, lena
    elif nr == 2:
        m.set_content('Ja, ich bringe sie mit.\n')
        m['Subject'], m['From'], m['To'], m['Cc'] = 'Re: ' + EIGEN_BETREFF, ich, jana, ', '.join([lena, tom])
        m['In-Reply-To'] = msgid('eigen-359-1')
        m['References'] = msgid('eigen-359-1')
    else:
        m.set_content('Erinnerung an mich selbst und Jana.\n')
        m['Subject'], m['From'], m['To'], m['Cc'] = EIGEN_EINGANG_BETREFF, ich, jana, ', '.join([ich, lena])
    m['Date'] = email.utils.format_datetime(
        datetime.datetime(2026, 10, 24, 10, {1: 20, 2: 40, 3: 50}[nr], tzinfo=datetime.timezone(datetime.timedelta(hours=2))))
    m['Message-ID'] = msgid('eigen-359-%d' % nr)
    return m


def antwort_eigen(c):
    """Fehlende Mails für „Antwort auf eigene Mail“ per APPEND (idempotent über die Message-ID)."""
    for nr, ziel in ((1, 'INBOX'), (2, 'Sent'), (3, 'INBOX')):
        name, _ = imap_ordner(c, ziel)
        c.select('"%s"' % name)
        if not c.uid('SEARCH', None, 'HEADER', 'Message-ID', msgid('eigen-359-%d' % nr))[1][0].split():
            print(c.append('"%s"' % name, '(\\Seen)', None, eigen_nachricht(nr).as_bytes()))
        print('EIGEN %d %s' % (nr, ziel))


def antwort(k, nur_kopf):
    c = imap_verbinden(k)
    c.select('INBOX', readonly=nur_kopf)
    uids = c.uid('SEARCH', None, 'HEADER', 'Message-ID', ANTWORT_MID)[1][0].split()
    if nur_kopf:
        if not uids:
            sys.exit('ABBRUCH: Testmail %s fehlt (--antwort)' % ANTWORT_MID)
        kopf = c.uid('FETCH', uids[0], '(BODY.PEEK[HEADER])')[1][0][1]
        m = email.message_from_bytes(kopf, policy=email.policy.default)
        for name in ('From', 'To', 'Cc', 'Reply-To', 'Subject', 'Message-ID', 'In-Reply-To', 'References'):
            if m[name] is not None:
                print('KOPF %s: %s' % (name, ' '.join(str(m[name]).split())))
    else:
        nur_testkonten([k.adresse])
        if not uids:
            print(c.append('INBOX', '(\\Seen)', None, antwort_nachricht().as_bytes()))
            uids = c.uid('SEARCH', None, 'HEADER', 'Message-ID', ANTWORT_MID)[1][0].split()
        antwort_eigen(c)
        print('ANTWORT %s/INBOX %s' % (k.account.get_uid(), ANTWORT_BETREFF))
    c.logout()
    return 0 if uids else 1


def sortierung(k, aktion):
    nur_testkonten([k.adresse])
    if k.adresse == KONTO_IMAP_1:
        sys.exit('ABBRUCH: --sortierung nicht in privat@ (Quota, #411)')
    if not k.goa.get_mail().props.imap_supported:
        sys.exit('ABBRUCH: --sortierung nur für IMAP')
    c = imap_verbinden(k)
    c.select('INBOX')

    def suche(betreff):
        return c.uid('SEARCH', None, 'SUBJECT', '"%s"' % betreff.replace(PREFIX + ' ', ''))[1][0].split()

    def ablegen(betreff, datum, gelesen, mid, antwort=False):
        m = EmailMessage(policy=email.policy.SMTP)
        m.set_content('%s (#418).\n' % betreff)
        m['Subject'] = ('Re: ' if antwort else '') + betreff
        m['From'] = email.utils.formataddr(('Revolution Testdaten', k.adresse))
        m['To'] = k.adresse
        m['Date'] = email.utils.formatdate(datum, localtime=True)
        m['Message-ID'] = msgid(mid)
        if antwort:
            m['In-Reply-To'] = m['References'] = msgid('sortierung-418-faden')
        print(c.append('INBOX', '(\\Seen)' if gelesen else None, imaplib.Time2Internaldate(datum), m.as_bytes()))

    if aktion == 'basis':
        if not suche(SORTIERUNG_BETREFF):
            ablegen(SORTIERUNG_BETREFF, datetime.datetime(2026, 10, 20, 9, 0).astimezone().timestamp(), False,
                    'sortierung-418-faden')
        if not suche(SORTIERUNG_OBEN):
            jetzt = time.time()
            ablegen(SORTIERUNG_OBEN, jetzt, True, 'sortierung-418-oben-%d' % jetzt)
    elif aktion == 'neu':
        jetzt = time.time()
        ablegen(SORTIERUNG_BETREFF, jetzt, False, 'sortierung-418-antwort-%d' % jetzt, antwort=True)
        ablegen(SORTIERUNG_NEU, jetzt - 1, False, 'sortierung-418-neu-%d' % jetzt)
    else:
        papierkorb = next(re.match(rb'.*" "?(.*?)"?$', z).group(1).decode() for z in c.list()[1]
                          if b'\\Trash' in z)
        for uid in suche(SORTIERUNG_BETREFF) + suche(SORTIERUNG_NEU) + suche(SORTIERUNG_OBEN):
            print('PAPIERKORB', uid.decode(), c.uid('MOVE', uid, '"%s"' % papierkorb))
        c.logout()
        return 0
    print('ORDNER %s/INBOX' % k.account.get_uid())
    for betreff in (SORTIERUNG_BETREFF, SORTIERUNG_NEU, SORTIERUNG_OBEN):
        for uid in suche(betreff):
            kopf = c.uid('FETCH', uid, '(BODY.PEEK[HEADER.FIELDS (DATE SUBJECT)])')[1][0][1].decode()
            datum = re.search(r'Date: (.*)', kopf).group(1).strip()
            rolle = betreff.split()[-1] if betreff != SORTIERUNG_BETREFF or 'Re:' not in kopf else 'Antwort'
            print('SORTIERUNG %s %s %s' % (rolle, uid.decode(), datum))
    c.logout()
    return 0


# #365: Suche. Ein Wort, das nur im Text dieser Mail steht (nicht in Betreff, Absender, Empfänger), in
# geops@ (Posteingang und Revolution-Test: „Alle Ordner“ findet beide) und outlook.com (Posteingang).
SUCHE_KONTEN = (KONTO_IMAP_2, KONTO_M365)
SUCHE_BETREFF = PREFIX + ' Suche Wochenmarkt'
SUCHE_WORT = 'Quittenbrotkiste'


def suche_nachricht():
    m = EmailMessage(policy=email.policy.SMTP)
    m.set_content('Testmail für die Suche in Revolution (#365). Am Samstag steht die %s neben der Waage.\n'
                  % SUCHE_WORT)
    m['Subject'] = SUCHE_BETREFF
    m['From'] = email.utils.formataddr(('Revolution Testdaten', ANZEIGE_ABSENDER))
    m['To'] = ANZEIGE_ABSENDER
    m['Date'] = 'Mon, 26 Oct 2026 09:00:00 +0100'
    m['Message-ID'] = msgid('suche-365')
    msg = Camel.MimeMessage.new()
    msg.construct_from_stream_sync(Camel.StreamMem.new_with_buffer(m.as_bytes()), None)
    return msg


def suche(konten, dry):
    """#365: Such-Mail per APPEND (idempotent, als gelesen); Zeilen „SUCHE <Adresse> <UID>/<Ordner>“."""
    for a in SUCHE_KONTEN:
        k = konten[a]
        nur_testkonten([a])
        ziele = [k.inbox]
        if k.goa.get_mail().props.imap_supported:
            ziele.append(ordner_holen(k.store, ORDNER, False))
        for f in ziele:
            f.refresh_info_sync(cancellable())
            if (SUCHE_BETREFF, ANZEIGE_ABSENDER) not in testmails(f):
                print('  %s: %s per APPEND in %s' % (a, SUCHE_BETREFF, f.get_full_name()))
                if dry:
                    continue
                info = Camel.MessageInfo.new(None)
                info.set_flags(Camel.MessageFlags.SEEN, Camel.MessageFlags.SEEN)
                f.append_message_sync(suche_nachricht(), info, cancellable())
                f.synchronize_sync(False, cancellable())
                f.refresh_info_sync(cancellable())
            print('SUCHE %s %s/%s' % (a, k.account.get_uid(), f.get_full_name()))


def bodystructure_anhang(teil, eltern=None):
    """#365: Anhang nach Camel (camel_content_disposition_is_attachment_ex, camel-mime-utils.c:3987-4048) für
    einen Teil aus IMAP BODYSTRUCTURE und seine Unterteile, wie imapx das Flag „Attachments“ setzt
    (camel-imapx-server.c:1078-1089, 1393-1400). teil: geparste Liste."""
    def s(x):
        return x.decode().lower() if isinstance(x, bytes) else ''

    def params(liste):
        return {s(liste[i]): liste[i + 1] for i in range(0, len(liste) - 1, 2)} if isinstance(liste, list) else {}

    if isinstance(teil[0], list):
        n = [i for i, x in enumerate(teil) if not isinstance(x, list)][0]
        typ, kinder, rest, name = ('multipart', s(teil[n])), teil[:n], teil[n + 1:], {}
        disp = rest[1] if len(rest) > 1 else None
    else:
        typ = (s(teil[0]), s(teil[1]))
        name = params(teil[2])
        basis = 7 + (1 if typ[0] == 'text' else 3 if typ == ('message', 'rfc822') else 0)
        disp = teil[basis + 1] if len(teil) > basis + 1 else None
        kinder = []
    disp = (s(disp[0]), params(disp[1])) if isinstance(disp, list) else None

    def ist():
        a, b = typ
        if a == 'application' and b in ('pkcs7-mime', 'xpkcs7mime', 'xpkcs7-mime', 'x-pkcs7-mime'):
            return False
        if typ == ('application', 'pgp-encrypted'):
            return eltern != ('multipart', 'encrypted')
        if typ == ('application', 'octet-stream') and eltern == ('multipart', 'encrypted'):
            return False
        if a == 'application' and b in ('pkcs7-signature', 'xpkcs7signature', 'xpkcs7-signature',
                                        'x-pkcs7-signature', 'pgp-signature'):
            return eltern != ('multipart', 'signed')
        if eltern and typ == ('message', 'rfc822'):
            return True
        if eltern == ('multipart', 'mixed') and not disp and ('name' in name or 'filename' in name):
            return True
        if not disp:
            return False
        if disp[0] == 'attachment':
            return True
        if disp[0] != 'inline' or a not in ('text', 'image'):
            return bool(disp[1].get('filename'))
        return False

    return ist() or any(bodystructure_anhang(k, typ) for k in kinder)


def imap_liste(daten):
    """#365: Antwort von UID FETCH (BODYSTRUCTURE) als verschachtelte Listen (Literale als Strings)."""
    roh = b''
    for x in daten:
        if isinstance(x, tuple):
            kopf = x[0][:x[0].rfind(b'{')]
            roh += kopf + b'"' + x[1].replace(b'\\', b'\\\\').replace(b'"', b'\\"') + b'"'
        elif isinstance(x, bytes):
            roh += x
    stapel, aktuell = [], []
    for t in re.findall(rb'\(|\)|"(?:\\.|[^"\\])*"|[^\s()"]+', roh):
        if t == b'(':
            stapel.append(aktuell)
            aktuell = []
        elif t == b')':
            fertig, aktuell = aktuell, stapel.pop()
            aktuell.append(fertig)
        elif t.startswith(b'"'):
            aktuell.append(re.sub(rb'\\(.)', rb'\1', t[1:-1]))
        else:
            aktuell.append(None if t.upper() == b'NIL' else t)
    return aktuell


def suche_zahlen(k, ziel):
    """#365: nur lesend, unabhängig von Camel: Treffer wie die Suche der App (mail-search.c) je Ordner. Text:
    IMAP UID SEARCH OR SUBJECT/FROM/TO/CC/BODY bzw. Graph $search; Filter: UNSEEN, FLAGGED (Graph isRead,
    importance high wie camel-m365-folder.c:659-660), Anhang aus BODYSTRUCTURE (bodystructure_anhang) bzw.
    Graph hasAttachments (camel-m365-folder.c:650-651). Ohne Gelöschte und Junk wie --anzahl. ziel „*“: alle
    Ordner des Kontos (LIST wie die Seitenleiste bei geops@ ohne Abos; Graph /me/messages), nur Text, Summe. Zeilen
    „SUCHE <UID>/<Ordner> text N ungelesen N markiert N anhang N“ bzw. „SUCHE <UID>/* text N“."""
    w = SUCHE_WORT
    if not k.goa.get_mail().props.imap_supported:
        def zaehle(pfad):
            n, url = 0, pfad
            while url:
                antwort = graph(k, 'GET', url)
                n += len(antwort['value'])
                url = antwort.get('@odata.nextLink', '').replace(GRAPH, '') or None
            return n
        if ziel == '*':
            print('SUCHE %s/* text %d' % (k.account.get_uid(),
                                          zaehle('/me/messages?$select=id&$top=100&$search=%22' + w + '%22')))
            return 0
        basis = '/me/mailFolders/inbox/messages?$select=id&$top=100'
        print('SUCHE %s/%s text %d ungelesen %d markiert %d anhang %d' % (
            k.account.get_uid(), ziel, zaehle(basis + '&$search=%22' + w + '%22'),
            zaehle(basis + '&$filter=isRead%20eq%20false'), zaehle(basis + '&$filter=importance%20eq%20%27high%27'),
            zaehle(basis + '&$filter=hasAttachments%20eq%20true')))
        return 0
    c = imap_verbinden(k)
    sichtbar = ['UNDELETED', 'UNKEYWORD', 'Junk', 'UNKEYWORD', '$Junk']
    text = ['OR', 'SUBJECT', w, 'OR', 'FROM', w, 'OR', 'TO', w, 'OR', 'CC', w, 'BODY', w]
    try:
        if ziel == '*':
            summe = 0
            for z in c.list('""', '*')[1]:
                m = re.match(rb'\((?P<flags>[^)]*)\) (?P<delim>"[^"]*"|NIL) (?P<name>.*)$', z) \
                    if isinstance(z, bytes) else None
                if not m or b'\\noselect' in m.group('flags').lower():
                    continue
                roh = m.group('name').decode()
                c.select(roh if roh.startswith('"') else '"%s"' % roh, readonly=True)
                n = len(c.uid('SEARCH', None, *(sichtbar + text))[1][0].split())
                if n:
                    print('TREFFER %s %d' % (roh, n))
                summe += n
            print('SUCHE %s/* text %d' % (k.account.get_uid(), summe))
            return 0
        name, _ = imap_ordner(c, ziel)
        c.select('"%s"' % name, readonly=True)
        alle = c.uid('SEARCH', None, *sichtbar)[1][0].split()
        anhang = 0
        if alle:
            daten = c.uid('FETCH', b','.join(alle).decode(), '(BODYSTRUCTURE)')[1]
            for eintrag in imap_liste(daten):
                if isinstance(eintrag, list):
                    i = [j for j, x in enumerate(eintrag) if x == b'BODYSTRUCTURE'][0]
                    anhang += bodystructure_anhang(eintrag[i + 1])
        print('SUCHE %s/%s text %d ungelesen %d markiert %d anhang %d' % (
            k.account.get_uid(), ziel, len(c.uid('SEARCH', None, *(sichtbar + text))[1][0].split()),
            len(c.uid('SEARCH', None, *(sichtbar + ['UNSEEN']))[1][0].split()),
            len(c.uid('SEARCH', None, *(sichtbar + ['FLAGGED']))[1][0].split()), anhang))
    finally:
        c.logout()
    return 0


def camel_nachricht(key, absender, betreff, an):
    m = baue(key)
    m['Subject'] = betreff
    m['From'] = absender
    m['To'] = ', '.join(an)
    m['Date'] = email.utils.formatdate(localtime=True)
    m['Message-ID'] = msgid(key)
    if key in ('thread2', 'thread3'):
        vorher = ['thread1'] if key == 'thread2' else ['thread1', 'thread2']
        m['In-Reply-To'] = msgid(vorher[-1])
        m['References'] = ' '.join(msgid(k) for k in vorher)
    data = m.as_bytes()
    msg = Camel.MimeMessage.new()
    msg.construct_from_stream_sync(Camel.StreamMem.new_with_buffer(data), None)
    return msg


# #352: Lastmails für die Nachrichtenliste. Absender erfunden (example.org, RFC 2606), nur als
# Kopfzeile; es wird nichts versendet. Datum alle 173 Minuten rückwärts (rund ein Jahr), damit
# alle Formen des relativen Datums vorkommen; jede 5. ungelesen, jede 11. markiert, jede 9. mit
# PNG-Anhang.
LAST_ABSENDER = ['Jana Albrecht', 'Tobias Krämer', 'Stadtwerke Lindenau', 'Miriam Okafor',
                 'Reisebüro Nordwind', 'Felix Brandt', 'Sophie Lange', 'Bücherstube am Markt',
                 'Hausverwaltung Kerner', 'Lukas Weber', 'Emma Schulze', 'Jonas Becker',
                 'Leonie Hoffmann', 'Paul Richter', 'Clara Neumann', 'Ben Wolf', 'Mia Schröder',
                 'Elias Zimmermann', 'Lina Braun', 'Noah Hartmann']
LAST_THEMEN = [
    ('Unterlagen für das Projekt Nordlicht', 'Hallo, anbei die überarbeiteten Pläne und die '
     'Kostenschätzung. Kannst du bis Freitag einen Blick darauf werfen?'),
    ('Kurze Frage zum Termin', 'Passt dir 14 Uhr statt 10 Uhr? Ich hänge morgens noch in einer '
     'Besprechung fest und würde sonst nur halb vorbereitet kommen.'),
    ('Ihre Rechnung', 'Sehr geehrte Kundin, sehr geehrter Kunde, Ihre Rechnung steht ab sofort im '
     'Kundenportal bereit. Den Abschlag haben wir angepasst.'),
    ('Fotos vom Sommerfest', 'Die Bilder sind wirklich schön geworden! Ich habe dir die Auswahl '
     'für die Vereinszeitung in den gemeinsamen Ordner gelegt.'),
    ('Protokoll der Sitzung', 'Liebe Mitglieder, hier das Protokoll vom Montag. Bitte gebt '
     'Korrekturen bis Ende der Woche durch.'),
    ('Abendessen am Samstag?', 'Wir haben spontan einen Tisch beim Italiener an der Ecke '
     'reserviert. Habt ihr Lust mitzukommen?'),
]


def last_nachricht(nr, jetzt):
    name = LAST_ABSENDER[nr % len(LAST_ABSENDER)]
    thema, text = LAST_THEMEN[nr % len(LAST_THEMEN)]
    m = EmailMessage(policy=email.policy.SMTP)
    m.set_content('%s\n\nLastmail %04d für die Nachrichtenliste (#352).\n' % (text, nr))
    if nr % 9 == 0:
        m.add_attachment(png_bytes(), maintype='image', subtype='png', filename='last-%04d.png' % nr)
    lokal = re.sub(r'[^a-z]+', '.', name.lower().translate(str.maketrans('äöüß', 'aous'))).strip('.')
    m['Subject'] = '%s Last %04d: %s' % (PREFIX, nr, thema)
    m['From'] = email.utils.formataddr((name, '%s@example.org' % lokal))
    m['To'] = LAST_KONTO
    m['Date'] = email.utils.format_datetime(jetzt - datetime.timedelta(minutes=173 * nr))
    m['Message-ID'] = '<revolution-last-%04d@%s>' % (nr, MSGID_DOMAIN)
    msg = Camel.MimeMessage.new()
    msg.construct_from_stream_sync(Camel.StreamMem.new_with_buffer(m.as_bytes()), None)
    return msg


def last(k, anzahl, dry, ordner=LAST_ORDNER):
    """#352: fehlende Lastmails 1..anzahl per APPEND in ordner (idempotent)."""
    nur_testkonten([k.adresse])
    folder = ordner_holen(k.store, ordner, not dry)
    if folder is None:
        print('  %s: Ordner %s anlegen, %d Lastmails einspielen' % (k.adresse, ordner, anzahl))
        return 0
    folder.refresh_info_sync(cancellable())
    vorhanden = set()
    for betreff, _ in testmails(folder):
        treffer = re.match(re.escape(PREFIX) + r' Last (\d{4}):', betreff)
        if treffer:
            vorhanden.add(int(treffer.group(1)))
    fehlt = [nr for nr in range(1, anzahl + 1) if nr not in vorhanden]
    print('  %s/%s: %d vorhanden, %d fehlen' % (k.adresse, folder.get_full_name(), len(vorhanden), len(fehlt)))
    jetzt = datetime.datetime.now().astimezone()
    for i, nr in enumerate(fehlt):
        if dry:
            break
        info = Camel.MessageInfo.new(None)
        flags = (0 if nr % 5 == 0 else Camel.MessageFlags.SEEN) | (Camel.MessageFlags.FLAGGED if nr % 11 == 0 else 0)
        info.set_flags(Camel.MessageFlags.SEEN | Camel.MessageFlags.FLAGGED, flags)
        # Je APPEND eigenes Zeitlimit; Timer danach beenden (sonst liefen Tausende Threads).
        c = Gio.Cancellable()
        t = threading.Timer(TIMEOUT, c.cancel)
        t.daemon = True
        t.start()
        try:
            folder.append_message_sync(last_nachricht(nr, jetzt), info, c)
        finally:
            t.cancel()
        if (i + 1) % 250 == 0:
            print('  %d von %d eingespielt' % (i + 1, len(fehlt)), flush=True)
    if fehlt and not dry:
        folder.synchronize_sync(False, cancellable())
        folder.refresh_info_sync(cancellable())
    print('LAST %s/%s %d' % (k.account.get_uid(), folder.get_full_name(), folder.get_message_count()))
    return 0


# ---------------------------------------------------------------- Camel/GOA

class TestSession(Camel.Session):
    """CamelSession: Passwort/Token aus GOA, kein Prompt."""

    def setup(self, goa_by_uid):
        self.goa_by_uid = goa_by_uid

    def passwort(self, service):
        obj = self.goa_by_uid[service.get_uid()]
        key = 'smtp-password' if isinstance(service, Camel.Transport) else 'imap-password'
        return obj.get_password_based().call_get_password_sync(key, None)

    def do_get_password(self, service, prompt, item, flags):
        return self.passwort(service)

    def do_get_oauth2_access_token_sync(self, service, c):
        obj = self.goa_by_uid[service.get_uid()]
        token, expires = obj.get_oauth2_based().call_get_access_token_sync(c)
        return True, token, expires

    def do_authenticate_sync(self, service, mechanism, c):
        # Microsoft365-SASL ist nur im Evolution-UI-Modul registriert (PM-Probe, außerhalb Repo);
        # dafür und für XOAUTH2 kein Passwort, sonst Passwort aus GOA setzen.
        authtype = Camel.Sasl.authtype(mechanism) if mechanism else None
        if (not mechanism or (authtype and authtype.need_password)) and not service.get_password():
            service.set_password(self.passwort(service))
        res = service.authenticate_sync(mechanism, c)
        if res == Camel.AuthenticationResult.ACCEPTED:
            return True
        raise GLib.Error('%s: Anmeldung %s abgelehnt (%s)'
                         % (service.get_display_name(), mechanism, res.value_nick))

    def do_get_filter_driver(self, type_, for_folder):
        # Leerer Filter: Evolutions Regeln gelten hier nicht.
        if not hasattr(self, 'filter'):
            self.filter = Camel.FilterDriver.new(self)
        return self.filter

    def do_trust_prompt(self, service, cert, errors):
        return Camel.CertTrust.REJECT_TEMPORARILY


class Konto:
    def __init__(self, adresse, goa_obj, account, transport):
        self.adresse = adresse
        self.goa = goa_obj
        self.account = account
        self.transport = transport
        self.store = None
        self.inbox = None


def konten_finden(registry):
    goa = {}
    for o in Goa.Client.new_sync(None).get_accounts():
        mail = o.get_mail()
        if mail and not o.get_account().props.mail_disabled:
            goa[o.get_account().props.id] = (mail.props.email_address.lower(), o)
    konten = {}
    for coll in registry.list_sources(EDataServer.SOURCE_EXTENSION_GOA):
        acc_id = coll.get_extension(EDataServer.SOURCE_EXTENSION_GOA).get_account_id()
        if acc_id not in goa or goa[acc_id][0] not in TESTKONTEN:
            continue
        adresse, obj = goa[acc_id]
        kids = [s for s in registry.list_sources(None) if s.get_parent() == coll.get_uid()]
        acc = [s for s in kids if s.has_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT)]
        tra = [s for s in kids if s.has_extension(EDataServer.SOURCE_EXTENSION_MAIL_TRANSPORT)]
        if acc and acc[0].get_enabled():
            konten[adresse] = Konto(adresse, obj, acc[0], tra[0] if tra else None)
    fehlt = [a for a in TESTKONTEN if a not in konten]
    if fehlt:
        sys.exit('ABBRUCH: Testkonto nicht in GOA/EDS: %s' % ', '.join(fehlt))
    return konten


def dienst(session, src, ext, typ):
    backend = src.get_extension(ext).get_backend_name()
    service = session.add_service(src.get_uid(), backend, typ)
    src.camel_configure_service(service)
    if isinstance(service, Camel.NetworkService):
        # host-reachable wird sonst erst über die Hauptschleife gesetzt (camel-network-service.c).
        service.can_reach_sync(cancellable())
    service.connect_sync(cancellable())
    if isinstance(service, Camel.OfflineStore):
        service.set_online_sync(True, cancellable())
    return service


class _CFI(ctypes.Structure):
    # GI liefert next/child von CamelFolderInfo nur als Zeiger-int; Layout laut camel-store.h
    # (Muster aus PM-Probeskript, außerhalb Repo).
    _fields_ = [('next', ctypes.c_void_p), ('parent', ctypes.c_void_p), ('child', ctypes.c_void_p),
                ('full_name', ctypes.c_char_p), ('display_name', ctypes.c_char_p),
                ('flags', ctypes.c_uint32), ('unread', ctypes.c_int32), ('total', ctypes.c_int32)]


def ordnerbaum(store):
    """[(full_name, flags)] wie Evolutions Ordnerbaum (RECURSIVE|SUBSCRIBED)."""
    fi = store.get_folder_info_sync(None, Camel.StoreGetFolderInfoFlags.RECURSIVE
                                    | Camel.StoreGetFolderInfoFlags.SUBSCRIBED, cancellable())
    knoten = [(fi.full_name, int(fi.flags))]

    def walk(addr):
        while addr:
            n = _CFI.from_address(addr)
            knoten.append((n.full_name.decode(), n.flags))
            walk(n.child)
            addr = n.next
    walk(fi.child)
    walk(fi.next)
    return knoten


def inbox_holen(store):
    try:
        inbox = store.get_inbox_folder_sync(cancellable())
        # imapx liefert hier eine Instanz außerhalb des Ordner-Caches des Stores; Abruf und
        # Flag-Abgleich laufen aber über die Cache-Instanz. Daher über get_folder_sync holen.
        return store.get_folder_sync(inbox.get_full_name(), 0, cancellable())
    except GLib.Error as e:
        if not e.matches(Camel.store_error_quark(), Camel.StoreError.NO_FOLDER):
            raise
    # microsoft365 hat kein get_inbox_folder_sync; Ordner mit Typ INBOX suchen.
    for name, flags in ordnerbaum(store):
        if flags & Camel.FOLDER_TYPE_MASK == int(Camel.FolderInfoFlags.TYPE_INBOX):
            return store.get_folder_sync(name, 0, cancellable())
    sys.exit('ABBRUCH: kein Posteingang in %s' % store.get_display_name())


def store_oeffnen(session, k):
    print('%s: verbinden' % k.adresse)
    k.store = dienst(session, k.account, EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT,
                     Camel.ProviderType.STORE)
    # imapx kennt Unterordner erst nach LIST (frischer Cache: „No such folder“);
    # wie Evolutions Ordner-Cache zuerst die Ordnerliste abrufen.
    ordnerbaum(k.store)
    k.inbox = inbox_holen(k.store)
    k.inbox.refresh_info_sync(cancellable())


def testmails(folder):
    """{(Betreff, Absender): uid} aller [Revolution-Test]-Mails im Ordner."""
    out = {}
    # Summary durchgehen statt search_by_expression: ohne (match-all …) liefert die Suche 0
    # (camel-folder-search.c, „TODO: else, find all matches“), mit match-all gibt PyGObject
    # die Ergebnis-Strings falsch frei („camel_pstring_free: String is not ours“).
    for uid in folder.get_uids():
        info = folder.get_message_info(uid)
        if info and (info.get_subject() or '').startswith(PREFIX):
            ia = Camel.InternetAddress.new()
            ia.decode(info.get_from())
            out[(info.get_subject(), ia.get(0)[2].lower())] = uid
    return out


def ordner_holen(store, name, anlegen):
    # df.eu legt alle Ordner unter INBOX an (Namespace „INBOX.“), daher auch dort suchen.
    for kandidat in (name, 'INBOX/' + name):
        try:
            return store.get_folder_sync(kandidat, 0, cancellable())
        except GLib.Error as e:
            if not e.matches(Camel.store_error_quark(), Camel.StoreError.NO_FOLDER):
                raise
    if not anlegen:
        return None
    parent, _, kurz = name.rpartition('/')
    eltern = ordner_holen(store, parent, False).get_full_name() if parent else None
    store.create_folder_sync(eltern, kurz, cancellable())
    folder = ordner_holen(store, name, False)
    voll = folder.get_full_name()
    if isinstance(store, Camel.Subscribable) and not store.folder_is_subscribed(voll):
        store.subscribe_folder_sync(voll, cancellable())
    return folder


# ---------------------------------------------------------------- Ablauf

def spam_retten(k, dry):
    """Testmails aus dem Spam-Ordner in den Posteingang verschieben (wie „Kein Spam“)."""
    junk = k.store.get_junk_folder_sync(cancellable())
    if junk is None or isinstance(junk, Camel.VeeFolder):
        # imapx: Junk ist ein virtueller Ordner; Gmail legt Spam in [Gmail]/Spam ab.
        junk = ordner_holen(k.store, '[Gmail]/Spam', False) if 'gmail' in k.adresse else None
    if junk is None:
        return set()
    junk.refresh_info_sync(cancellable())
    uids = list(testmails(junk).items())
    for (betreff, absender), uid in uids:
        print('  %s: %s (von %s) aus %s -> Posteingang' % (k.adresse, betreff, absender,
                                                           junk.get_full_name()))
        if not dry:
            junk.transfer_messages_to_sync([uid], k.inbox, True, cancellable())
    if uids and not dry:
        junk.synchronize_sync(True, cancellable())
        k.inbox.refresh_info_sync(cancellable())
    return {key for key, _ in uids}


def senden(session, konten, dry):
    vorhanden = {a: spam_retten(k, dry) | set(testmails(k.inbox)) for a, k in konten.items()}
    transporte = {}
    gesendet = 0
    for key, absender, betreff, _ in MAILS:
        an = [a for a in TESTKONTEN if (betreff, absender) not in vorhanden[a]]
        if not an:
            continue
        nur_testkonten(an + [absender])
        print('  senden %-10s %s -> %s' % (key, absender, ', '.join(an)))
        gesendet += 1
        if dry:
            continue
        if absender not in transporte:
            transporte[absender] = dienst(session, konten[absender].transport,
                                          EDataServer.SOURCE_EXTENSION_MAIL_TRANSPORT,
                                          Camel.ProviderType.TRANSPORT)
        von = Camel.InternetAddress.new()
        von.add('', absender)
        empf = Camel.InternetAddress.new()
        for a in an:
            empf.add('', a)
        msg = camel_nachricht(key, absender, betreff, an)
        transporte[absender].send_to_sync(msg, von, empf, cancellable())
        time.sleep(2)  # Date-Reihenfolge im Thread
    for t in transporte.values():
        t.disconnect_sync(True, None)
    return gesendet


def warten(konten):
    soll = {(b, a) for _, a, b, _ in MAILS}
    ende = time.monotonic() + WARTEN
    while True:
        offen = {}
        for a, k in konten.items():
            k.inbox.refresh_info_sync(cancellable())
            spam_retten(k, False)
            fehlt = soll - set(testmails(k.inbox))
            if fehlt:
                offen[a] = fehlt
        if not offen:
            return
        if time.monotonic() > ende:
            for a, f in offen.items():
                print('  FEHLT in %s/INBOX: %s' % (a, sorted(b for b, _ in f)))
            sys.exit('ABBRUCH: Zustellung nach %d s unvollständig' % WARTEN)
        time.sleep(15)


def ablegen(konten, dry):
    for a, k in konten.items():
        nur_testkonten([a])
        mails = testmails(k.inbox)
        for key, absender, betreff, gelesen in MAILS:
            uid = mails.get((betreff, absender))
            if uid is None:
                continue
            info = k.inbox.get_message_info(uid)
            ist = bool(info.get_flags() & Camel.MessageFlags.SEEN)
            if ist != gelesen:
                print('  %s: %s -> %s' % (a, betreff, 'gelesen' if gelesen else 'ungelesen'))
                if not dry:
                    info.set_flags(Camel.MessageFlags.SEEN, Camel.MessageFlags.SEEN if gelesen else 0)
        if not dry:
            k.inbox.synchronize_sync(False, cancellable())
        ordner = {}
        for name in (ORDNER, UNTERORDNER):
            ordner[name] = ordner_holen(k.store, name, not dry)
            if ordner[name] is None:
                print('  %s: Ordner %s anlegen' % (a, name))
        for key, ziel in KOPIEN.items():
            _, absender, betreff, _ = next(m for m in MAILS if m[0] == key)
            uid = mails.get((betreff, absender))
            dest = ordner[ziel]
            if dest is not None:
                dest.refresh_info_sync(cancellable())
                if (betreff, absender) in testmails(dest):
                    continue
            print('  %s: %s -> %s kopieren' % (a, betreff, ziel))
            if not dry and uid is not None:
                k.inbox.transfer_messages_to_sync([uid], dest, False, cancellable())
                dest.synchronize_sync(False, cancellable())


def teile(part, tiefe, out):
    dw = part.get_content()
    ct = part.get_content_type()
    typ = '%s/%s' % (ct.type, ct.subtype)
    if isinstance(dw, Camel.Multipart):
        out.append((tiefe, typ, None, None))
        for i in range(dw.get_number()):
            teile(dw.get_part(i), tiefe + 1, out)
        return
    extra = ct.param('method') if typ == 'text/calendar' else None
    if typ == 'text/html':
        stream = Camel.StreamMem.new()
        dw.decode_to_stream_sync(stream, None)
        html = bytes(stream.get_byte_array()).decode('utf-8', 'replace')
        extra = 'Remote-Bild=%s' % ('ja' if 'src="%s"' % REMOTE_BILD in html else 'nein')
    out.append((tiefe, typ, part.get_filename(), extra))


def pruefen(konten):
    fehler = 0
    for a, k in konten.items():
        print('=' * 70)
        print('%s  (Quelle %s, Backend %s)' % (a, k.account.get_uid()[:8],
              k.account.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_backend_name()))
        k.inbox.refresh_info_sync(cancellable())
        mails = testmails(k.inbox)
        print('  %s: %d [Revolution-Test]-Mails' % (k.inbox.get_full_name(), len(mails)))
        for key, absender, betreff, gelesen in MAILS:
            uid = mails.get((betreff, absender))
            if uid is None:
                print('  FEHLT  %s (%s)' % (betreff, absender))
                fehler += 1
                continue
            seen = bool(k.inbox.get_message_info(uid).get_flags() & Camel.MessageFlags.SEEN)
            ok = seen == gelesen
            fehler += not ok
            print('  %s %-9s %s  von %s' % ('ok  ' if ok else 'FLAG', 'gelesen' if seen else 'ungelesen',
                                           betreff, absender))
            if key in ('html', 'anhang', 'thread3', 'einladung'):
                msg = k.inbox.get_message_sync(uid, cancellable())
                if key == 'thread3':
                    print('         In-Reply-To=%s References=%s' % (
                        msg.get_header('In-Reply-To'), (msg.get_header('References') or '').split()))
                    continue
                out = []
                teile(msg, 0, out)
                for tiefe, typ, name, extra in out:
                    print('         %s%s%s%s' % ('  ' * tiefe, typ, ' name=%s' % name if name else '',
                                                 ' %s' % extra if extra else ''))
        for key, betreff in ANZEIGE if a in ANZEIGE_KONTEN else ():
            # #411: Absender je Schlüssel wie in anzeige() (#355: anderer Absender)
            absender = ANZEIGE_ANDERE.get(key, ANZEIGE_ABSENDER)
            uid = mails.get((betreff, absender))
            if uid is None:
                print('  FEHLT  %s (%s)' % (betreff, absender))
                fehler += 1
                continue
            print('  ok   %s  UID %s' % (betreff, uid))
            out = []
            teile(k.inbox.get_message_sync(uid, cancellable()), 0, out)
            for tiefe, typ, name, extra in out:
                print('         %s%s%s' % ('  ' * tiefe, typ, ' name=%s' % name if name else ''))
        for name in (ORDNER, UNTERORDNER):
            f = ordner_holen(k.store, name, False)
            if f is None:
                print('  FEHLT  Ordner %s' % name)
                fehler += 1
                continue
            f.refresh_info_sync(cancellable())
            sub = f.get_full_name() in [n for n, _ in ordnerbaum(k.store)]
            print('  Ordner %-34s im Ordnerbaum=%s  Testmails: %s' % (
                f.get_full_name(), sub, sorted(b for b, _ in testmails(f))))
            fehler += not sub
    print('=' * 70)
    print('Prüfung: %d Fehler' % fehler)
    return fehler


def ordner_konten(registry):
    """#348: alle aktivierten Mail-Konten aus GOA wie die Seitenleiste (Online-Konto mit Mail,
    Quelle und Collection aktiviert, mail-account.c), ohne Einschränkung auf Testkonten."""
    goa = {}
    for o in Goa.Client.new_sync(None).get_accounts():
        mail = o.get_mail()
        if mail and not o.get_account().props.mail_disabled:
            goa[o.get_account().props.id] = (mail.props.email_address.lower(), o)
    konten = []
    # #415: wie die App (mail-account.c) Konten aus REVOLUTION_SKIP_ACCOUNTS auslassen.
    skip = os.environ.get('REVOLUTION_SKIP_ACCOUNTS', '').split(',') \
        if os.environ.get('REVOLUTION_TEST_HOOKS') == '1' else []
    for coll in registry.list_sources(EDataServer.SOURCE_EXTENSION_GOA):
        acc_id = coll.get_extension(EDataServer.SOURCE_EXTENSION_GOA).get_account_id()
        if acc_id not in goa or not coll.get_enabled():
            continue
        for s in registry.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT):
            if s.get_parent() == coll.get_uid() and s.get_enabled() and s.get_uid() not in skip:
                konten.append(Konto(goa[acc_id][0], goa[acc_id][1], s, None))
    return konten


def ordner(session, konten):
    """#348: nur lesend. Verbinden und camel_store_get_folder_info_sync mit denselben Flags
    wie die App (RECURSIVE|SUBSCRIBED, main.c note_store_thread); keine Schreibzugriffe."""
    for k in konten:
        store = dienst(session, k.account, EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT,
                       Camel.ProviderType.STORE)
        namen = [n for n, _ in ordnerbaum(store)]
        for n in namen:
            print('ORDNER %s %s' % (k.account.get_uid(), n))
        print('ANZAHL %s %d %s' % (k.account.get_uid(), len(namen), k.adresse))
        store.disconnect_sync(True, None)
    return 0


# ---------------------------------------------------------------- #353: Flags direkt am Server

GRAPH = 'https://graph.microsoft.com/v1.0'
EINLADUNG = PREFIX + ' Einladung: Abstimmung Testdaten'
LIVE = PREFIX + ' Live'


def imap_verbinden(k):
    """Eigene IMAP-Verbindung mit den GOA-Daten (Host, Benutzer, Passwort bzw. OAuth2)."""
    m = k.goa.get_mail()
    host, _, port = m.props.imap_host.partition(':')
    if m.props.imap_use_ssl:
        c = imaplib.IMAP4_SSL(host, int(port or 993), timeout=TIMEOUT)
    else:
        c = imaplib.IMAP4(host, int(port or 143), timeout=TIMEOUT)
        if m.props.imap_use_tls:
            c.starttls()
    user = m.props.imap_user_name
    if k.goa.get_oauth2_based():
        token = k.goa.get_oauth2_based().call_get_access_token_sync(None)[0]
        c.authenticate('XOAUTH2', lambda _: ('user=%s\x01auth=Bearer %s\x01\x01' % (user, token)).encode())
    else:
        c.login(user, k.goa.get_password_based().call_get_password_sync('imap-password', None))
    return c


def imap_ordner(c, ziel=ORDNER):
    """(IMAP-Name, Camel-Name) des Ordners Revolution-Test (df.eu: unter INBOX mit Trenner „.“)."""
    if ziel == 'INBOX':
        return 'INBOX', 'INBOX'
    typ, daten = c.list()
    for zeile in daten:
        treffer = re.match(rb'\((.*?)\) "(.*?)" "?(.*?)"?$', zeile)
        if treffer and treffer.group(3).decode().split(treffer.group(2).decode())[-1] == ziel:
            name = treffer.group(3).decode()
            return name, name.replace(treffer.group(2).decode(), '/')
    sys.exit('ABBRUCH: Ordner %s fehlt' % ziel)


def imap_mails(c):
    """[(UID, gelesen, markiert, Betreff)] der Testmails im gewählten Ordner."""
    typ, daten = c.uid('SEARCH', None, 'ALL')
    out = []
    for uid in daten[0].split():
        typ, d = c.uid('FETCH', uid, '(FLAGS BODY.PEEK[HEADER.FIELDS (SUBJECT)])')
        flags = d[0][0].decode()
        betreff = str(email.header.make_header(email.header.decode_header(
            d[0][1].decode().partition(':')[2].strip())))
        if betreff.startswith(PREFIX):
            out.append((uid.decode(), '\\Seen' in flags, '\\Flagged' in flags, betreff))
    return out


def graph(k, methode, pfad, daten=None):
    token = k.goa.get_oauth2_based().call_get_access_token_sync(None)[0]
    req = urllib.request.Request(GRAPH + pfad, method=methode,
                                 data=json.dumps(daten).encode() if daten is not None else None,
                                 headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return json.loads(r.read() or b'null')


def graph_mails(k):
    """(Ordner-ID, [(ID, gelesen, markiert, Betreff, flagStatus)]) in Revolution-Test."""
    ordner = graph(k, 'GET', "/me/mailFolders?$top=100&$filter=displayName%20eq%20'" + ORDNER + "'")['value']
    if not ordner:
        sys.exit('ABBRUCH: Ordner %s fehlt' % ORDNER)
    mails = graph(k, 'GET', '/me/mailFolders/%s/messages?$top=100&$select=subject,isRead,importance,flag'
                  % ordner[0]['id'])['value']
    return ordner[0]['id'], [(m['id'], m['isRead'], m['importance'] == 'high', m['subject'],
                              m['flag']['flagStatus']) for m in mails if m['subject'].startswith(PREFIX)]


def flags(k):
    """#353: nur lesend (IMAP EXAMINE, Graph GET)."""
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        name, camel = imap_ordner(c)
        c.select('"%s"' % name, readonly=True)
        print('ORDNER %s/%s' % (k.account.get_uid(), camel))
        for uid, seen, flagged, betreff in imap_mails(c):
            print('FLAGS %d %d %s' % (seen, flagged, betreff))
        c.logout()
    else:
        _, mails = graph_mails(k)
        print('ORDNER %s/%s' % (k.account.get_uid(), ORDNER))
        for _, seen, flagged, betreff, status in mails:
            print('FLAGS %d %d %s (flag.flagStatus=%s)' % (seen, flagged, betreff, status))
    return 0


def ungelesen(k, ziel, soll=None):
    """#353: ungelesene UIDs eines IMAP-Ordners; mit soll (Datei mit einer früheren Ausgabe)
    diesen Stand wiederherstellen."""
    c = imap_verbinden(k)
    name, _ = imap_ordner(c, ziel)
    c.select('"%s"' % name, readonly=soll is None)
    ist = set(c.uid('SEARCH', None, 'UNSEEN')[1][0].split())
    if soll is not None:
        nur_testkonten([k.adresse])
        zeile = next(z for z in open(soll) if z.startswith('UNGELESEN '))
        ziel_uids = {u.encode() for u in zeile.split()[2:]}
        for uids, aenderung in ((ist - ziel_uids, '+FLAGS'), (ziel_uids - ist, '-FLAGS')):
            if uids:
                menge = b','.join(sorted(uids, key=int)).decode()
                print('%s \\Seen %s: %s' % (aenderung, menge, c.uid('STORE', menge, aenderung, '(\\Seen)')[0]))
        ist = set(c.uid('SEARCH', None, 'UNSEEN')[1][0].split())
    c.logout()
    print('UNGELESEN %d %s' % (len(ist), ' '.join(sorted((u.decode() for u in ist), key=int))))
    return 0


def server(k, aktion, ziel=ORDNER, warte=None):
    """#353: serverseitige Änderung in einem Testkonto, während die App läuft; neu/weg auch im
    Posteingang (ziel INBOX, Beleg für IDLE)."""
    nur_testkonten([k.adresse])
    if k.goa.get_mail().props.imap_supported:
        if ziel == 'Papierkorb':
            sys.exit('ABBRUCH: Papierkorb nur für Graph')
        c = imap_verbinden(k)
        name, _ = imap_ordner(c, ziel)
        c.select('"%s"' % name)
        if warte:
            print('BEREIT', flush=True)
            while not os.path.exists(warte):
                time.sleep(0.005)
        if aktion == 'neu':
            m = EmailMessage(policy=email.policy.SMTP)
            m.set_content('Live-Mail für #353 (APPEND, wird danach gelöscht).\n')
            m['Subject'] = '%s %s' % (LIVE, datetime.datetime.now().strftime('%H:%M:%S'))
            m['From'] = k.adresse
            m['To'] = k.adresse
            m['Date'] = email.utils.formatdate(localtime=True)
            print(c.append('"%s"' % name, None, None, m.as_bytes()))
            # #411: eindeutiger Betreff für tests/kombiniert.sh (REVOLUTION_MESSAGE)
            print('BETREFF %s' % m['Subject'])
            if warte:
                print('ABGELEGT ' + datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3], flush=True)
        elif aktion == 'weg':
            # #366: nur die eigenen UIDs endgültig (UID EXPUNGE), nie einfaches EXPUNGE: das löschte auch
            # \Deleted-Mails paralleler Läufe (Regel PM 27.10.2026).
            for uid, _, _, betreff in imap_mails(c):
                if betreff.startswith(LIVE):
                    print('BEFEHL UID STORE %s +FLAGS (\\Deleted) → %s' % (uid, c.uid('STORE', uid, '+FLAGS', '(\\Deleted)')[0]))
                    print('BEFEHL UID EXPUNGE %s → %s' % (uid, c.uid('EXPUNGE', uid)[0]))
        else:
            uid = next(u for u, _, _, b in imap_mails(c) if b == EINLADUNG)
            flag = '\\Seen' if aktion in ('gelesen', 'ungelesen') else '\\Flagged'
            print(c.uid('STORE', uid, '+FLAGS' if aktion in ('gelesen', 'markiert') else '-FLAGS', '(%s)' % flag))
        c.logout()
    else:
        if aktion in ('neu', 'weg'):
            sys.exit('ABBRUCH: %s nur für IMAP' % aktion)
        if ziel == 'Papierkorb':
            # #416: Einladung, solange tests/verschieben.sh sie in den Papierkorb verschoben hat.
            mails = graph(k, 'GET', '/me/mailFolders/deleteditems/messages?$select=subject&$filter='
                          + urllib.parse.quote("subject eq '%s'" % EINLADUNG))['value']
            mid = mails[0]['id']
        else:
            _, mails = graph_mails(k)
            mid = next(m[0] for m in mails if m[3] == EINLADUNG)
        daten = {'gelesen': {'isRead': True}, 'ungelesen': {'isRead': False},
                 'markiert': {'importance': 'high'}, 'unmarkiert': {'importance': 'normal'}}[aktion]
        graph(k, 'PATCH', '/me/messages/' + mid, daten)
        print('PATCH %s' % daten)
    return 0


def benachrichtigung(k, aktion, arg):
    """#366: Testmails im Posteingang für Benachrichtigungen; löschen nur genau die eigenen UIDs."""
    nur_testkonten([k.adresse])
    if not k.goa.get_mail().props.imap_supported:
        sys.exit('ABBRUCH: --benachrichtigung nur für IMAP')
    c = imap_verbinden(k)
    if aktion == 'neu':
        for name in arg.split(','):
            m = EmailMessage(policy=email.policy.SMTP)
            m.set_content('Testmail für #366 (Benachrichtigung), wird nach dem Test gelöscht.\n')
            m['Subject'] = '%s Benachrichtigung %s %s' % (PREFIX, name, datetime.datetime.now().strftime('%H:%M:%S'))
            m['From'] = 'Revolution Test <%s>' % k.adresse
            m['To'] = k.adresse
            m['Date'] = email.utils.formatdate(localtime=True)
            m['Message-ID'] = email.utils.make_msgid()
            typ, daten = c.append('INBOX', '(\\Seen)' if name.startswith('gelesen') else None, None, m.as_bytes())
            treffer = re.search(rb'APPENDUID (\d+) (\d+)', daten[0] or b'')
            if typ != 'OK' or not treffer:
                sys.exit('ABBRUCH: APPEND ohne APPENDUID: %r' % daten)
            print('NEU %s %s %s' % (treffer.group(1).decode(), treffer.group(2).decode(), m['Subject']), flush=True)
    elif aktion == 'gelesen':
        # Eine eigene Testmail (UID) am Server als gelesen markieren, Betreff geprüft.
        c.select('INBOX')
        typ, d = c.uid('FETCH', arg, '(BODY.PEEK[HEADER.FIELDS (SUBJECT)])')
        ist = str(email.header.make_header(email.header.decode_header(d[0][1].decode().partition(':')[2].strip()))) \
            if typ == 'OK' and d and d[0] is not None else ''
        if not ist.startswith(PREFIX + ' Benachrichtigung'):
            sys.exit('ABBRUCH: UID %s ist keine Testmail (%r), nichts geändert' % (arg, ist))
        print('BEFEHL UID STORE %s +FLAGS (\\Seen) → %s' % (arg, c.uid('STORE', arg, '+FLAGS', '(\\Seen)')[0]))
    else:
        c.select('INBOX')
        gueltig = c.response('UIDVALIDITY')[1][0].decode()
        with open(arg, encoding='utf-8') as f:
            eigene = [z.rstrip('\n').split(' ', 3)[1:] for z in f if z.startswith('NEU ')]
        for validity, uid, betreff in eigene:
            if validity != gueltig:
                sys.exit('ABBRUCH: UIDVALIDITY %s statt %s, nichts gelöscht' % (gueltig, validity))
            typ, d = c.uid('FETCH', uid, '(BODY.PEEK[HEADER.FIELDS (SUBJECT)])')
            if typ != 'OK' or not d or d[0] is None:
                print('WEG %s schon fort' % uid)
                continue
            ist = str(email.header.make_header(email.header.decode_header(d[0][1].decode().partition(':')[2].strip())))
            if ist != betreff or not ist.startswith(PREFIX + ' Benachrichtigung'):
                sys.exit('ABBRUCH: UID %s hat Betreff %r statt %r, nichts gelöscht' % (uid, ist, betreff))
            print('BEFEHL UID STORE %s +FLAGS (\\Deleted) → %s' % (uid, c.uid('STORE', uid, '+FLAGS', '(\\Deleted)')[0]))
            print('BEFEHL UID EXPUNGE %s → %s' % (uid, c.uid('EXPUNGE', uid)[0]))
    c.logout()
ENTWURF = PREFIX + ' Entwurf 360'


def imap_entwuerfe(c):
    """#360: Entwürfe-Ordner per LIST (Attribut \\Drafts, RFC 6154), sonst Name Drafts/Entwürfe."""
    kandidaten = []
    for zeile in c.list('""', '*')[1]:
        treffer = re.match(rb'\((.*?)\) "(.*?)" "?(.*?)"?$', zeile)
        if not treffer:
            continue
        name = treffer.group(3).decode()
        if b'\\Drafts' in treffer.group(1):
            return name
        if mutf7_decode(name).split(treffer.group(2).decode())[-1] in ('Drafts', 'Entwürfe'):
            kandidaten.append(name)
    if not kandidaten:
        sys.exit('ABBRUCH: kein Entwürfe-Ordner')
    return kandidaten[0]


KONTAKT_KONTO = KONTO_GOOGLE
KONTAKT_NAME = 'Revolution-Test Kontakt 361'
KONTAKT_ADRESSE = HILFS_KONTAKT361


def kontakt(registry, aktion):
    """#361: siehe Kopf (--kontakt). Schreibt nur in das Adressbuch unter der GOA-Sammlung des Testkontos."""
    gi.require_version('EBook', '1.2')
    gi.require_version('EBookContacts', '1.2')
    from gi.repository import EBook, EBookContacts
    nur_testkonten([KONTAKT_KONTO])
    sammlung = konten_finden(registry)[KONTAKT_KONTO].account.get_parent()
    buecher = [b for b in registry.list_sources(EDataServer.SOURCE_EXTENSION_ADDRESS_BOOK)
               if b.get_parent() == sammlung]
    if len(buecher) != 1:
        sys.exit('ABBRUCH: %d Adressbücher unter %s' % (len(buecher), KONTAKT_KONTO))
    print('ADRESSBUCH %s %s' % (buecher[0].get_uid(), buecher[0].get_display_name()))
    c = EBook.BookClient.connect_sync(buecher[0], 30, cancellable())
    abfrage = '(is "full_name" "%s")' % KONTAKT_NAME
    vorhanden = c.get_contacts_sync(abfrage, cancellable())[1]
    if aktion == 'anlegen' and not vorhanden:
        neu = EBookContacts.Contact.new()
        neu.set_property('full-name', KONTAKT_NAME)
        neu.set_property('email-1', KONTAKT_ADRESSE)
        neu.set_property('email-2', KONTAKT_ADRESSE.replace('@', 'b@'))  # zweite Zeile für die Pfeiltasten
        print('ANGELEGT %s' % c.add_contact_sync(neu, EBookContacts.BookOperationFlags.NONE, cancellable())[1])
        vorhanden = c.get_contacts_sync(abfrage, cancellable())[1]
    for x in vorhanden:
        print('KONTAKT %s %s %s' % (x.get_property('id'), x.get_property('full-name'), x.get_property('email-1')))
        if aktion == 'weg' and x.get_property('full-name').startswith('Revolution-Test'):
            c.remove_contact_by_uid_sync(x.get_property('id'), EBookContacts.BookOperationFlags.NONE, cancellable())
            print('GELOESCHT %s' % x.get_property('id'))
    return 0


# #368: Schreiben nur in diese Adressbücher (Briefing Zusatz M6, 28.10.2026).
KONTAKT_SCHREIBBAR = ('518e93226d14866ae0d8074ec0128a43c0c0c257', '3be9d2a4067fd00b72a3d3d5dbd529075a08ea88',
                      'system-address-book')
KONTAKT_PREFIX = 'Revolution-Test'
ALLE_KONTAKTE = '(contains "x-evolution-any-field" "")'


def ebook():
    gi.require_version('EBook', '1.2')
    gi.require_version('EBookContacts', '1.2')
    from gi.repository import EBook, EBookContacts
    return EBook, EBookContacts


def adressbuecher(registry):
    """#368: siehe Kopf (--adressbuecher), nur lesend."""
    EBook, _ = ebook()
    for b in registry.list_enabled(EDataServer.SOURCE_EXTENSION_ADDRESS_BOOK):
        eltern = registry.ref_source(b.get_parent()) if b.get_parent() else None
        konto = (eltern or b).get_display_name()
        c = EBook.BookClient.connect_sync(b, 0xffffffff, cancellable())
        initial = c.check_capability('do-initial-query')
        n, listen = '-', '-'
        if initial:
            alle = c.get_contacts_sync(ALLE_KONTAKTE, cancellable())[1]
            listen = sum(1 for x in alle if x.get_property('list'))
            n = len(alle) - listen
        print('ADRESSBUCH %s %s / %s nurlesend %d initial %d kontakte %s listen %s'
              % (b.get_uid(), konto, b.get_display_name(), c.is_readonly(), initial, n, listen), flush=True)
    return 0


def kontakt_live(registry, aktion, buch, datei):
    """#368: siehe Kopf (--kontakt-live). Ändern/Löschen nur über die gespeicherte UID, nie per Suche."""
    EBook, EBookContacts = ebook()
    if aktion != 'pruefen' and buch not in KONTAKT_SCHREIBBAR:
        sys.exit('ABBRUCH: Adressbuch %s ist nicht zum Schreiben freigegeben' % buch)
    quelle = registry.ref_source(buch)
    if quelle is None:
        sys.exit('ABBRUCH: Adressbuch %s fehlt' % buch)
    c = EBook.BookClient.connect_sync(quelle, 0xffffffff, cancellable())
    if aktion == 'refresh':
        c.refresh_sync(cancellable())
        print('REFRESH %s' % buch, flush=True)
        return 0
    if aktion == 'anlegen':
        zeit = datetime.datetime.now().strftime('%H%M%S')
        name = EBookContacts.ContactName.new()
        name.given = 'Live'
        name.family = '%s 368 %s' % (KONTAKT_PREFIX, zeit)
        neu = EBookContacts.Contact.new()
        neu.set_property('name', name)
        neu.set_property('full-name', 'Live %s' % name.family)
        neu.set_property('file-as', '%s, Live' % name.family)  # microsoft365 verlangt file_as
        uid = c.add_contact_sync(neu, EBookContacts.BookOperationFlags.NONE, cancellable())[1]
        with open(datei, 'w') as f:
            f.write('KONTAKT %s %s %s\n' % (buch, uid, neu.get_property('full-name')))
        print('SCHREIBVORGANG %s' % buch)
        print('ANGELEGT %s %s' % (uid, neu.get_property('full-name')), flush=True)
        return 0
    zeile = open(datei).read().split(None, 3)
    if len(zeile) < 3 or zeile[0] != 'KONTAKT' or zeile[1] != buch:
        sys.exit('ABBRUCH: %s enthält keinen Testkontakt aus %s' % (datei, buch))
    uid = zeile[2]
    try:
        x = c.get_contact_sync(uid, cancellable())[1]
    except GLib.Error as e:
        if aktion == 'pruefen' and 'not found' in e.message.lower():
            print('VORHANDEN 0 %s' % uid, flush=True)
            return 0
        raise
    if aktion == 'pruefen':
        print('VORHANDEN 1 %s %s' % (uid, x.get_property('full-name')), flush=True)
        return 0
    if not (x.get_property('name') and (x.get_property('name').family or '').startswith(KONTAKT_PREFIX)):
        sys.exit('ABBRUCH: Kontakt %s hat keinen Nachnamen %s…' % (uid, KONTAKT_PREFIX))
    if aktion == 'aendern':
        x.set_property('full-name', x.get_property('full-name') + ' geändert')
        c.modify_contact_sync(x, EBookContacts.BookOperationFlags.NONE, cancellable())
        print('SCHREIBVORGANG %s' % buch)
        print('GEAENDERT %s %s' % (uid, x.get_property('full-name')), flush=True)
    elif aktion == 'loeschen':
        c.remove_contact_by_uid_sync(uid, EBookContacts.BookOperationFlags.NONE, cancellable())
        print('SCHREIBVORGANG %s' % buch)
        print('GELOESCHT %s %s' % (uid, x.get_property('full-name')), flush=True)
    return 0


def kontakt_graph(k, aktion, datei):
    """#368: siehe Kopf (--kontakt-graph); nur KONTO_M365, nur die gespeicherte ID."""
    nur_testkonten([k.adresse])
    if aktion == 'anlegen':
        nachname = '%s 368 Graph %s' % (KONTAKT_PREFIX, datetime.datetime.now().strftime('%H%M%S'))
        neu = graph(k, 'POST', '/me/contacts', {'givenName': 'Server', 'surname': nachname,
                                                'fileAs': '%s, Server' % nachname})
        with open(datei, 'w') as f:
            f.write('GRAPH %s %s\n' % (neu['id'], neu.get('displayName')))
        print('SCHREIBVORGANG graph')
        print('ANGELEGT %s %s' % (neu['id'], neu.get('displayName')), flush=True)
        return 0
    zeile = open(datei).read().split(None, 2)
    if len(zeile) < 2 or zeile[0] != 'GRAPH':
        sys.exit('ABBRUCH: %s enthält keine Graph-ID' % datei)
    x = graph(k, 'GET', '/me/contacts/%s?$select=surname,displayName' % zeile[1])
    if not (x.get('surname') or '').startswith(KONTAKT_PREFIX):
        sys.exit('ABBRUCH: Graph-Kontakt %s hat keinen Nachnamen %s…' % (zeile[1], KONTAKT_PREFIX))
    graph(k, 'DELETE', '/me/contacts/%s' % zeile[1])
    print('SCHREIBVORGANG graph')
    print('GELOESCHT %s %s' % (zeile[1], x.get('displayName')), flush=True)
    try:
        graph(k, 'GET', '/me/contacts/%s?$select=surname' % zeile[1])
        print('NOCH-DA %s' % zeile[1])
    except urllib.error.HTTPError as e:
        print('WEG %s HTTP %d' % (zeile[1], e.code), flush=True)
    return 0


def entwurf(k, betreff, weg):
    """#360: Entwürfe mit genau diesem Betreff (je Lauf eindeutig) am Server zählen; weg = Message-IDs aus dem
    App-Log („Entwurf: gespeichert …“), nur diese Entwürfe endgültig löschen (eigene Testdaten)."""
    nur_testkonten([k.adresse])
    if not betreff.startswith(ENTWURF):
        sys.exit('ABBRUCH: Betreff ohne %s' % ENTWURF)
    ids = [m if m.startswith('<') else '<%s>' % m for m in weg]
    sichtbar = geloescht = 0
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        name = imap_entwuerfe(c)
        print('ENTWURFORDNER %s' % mutf7_decode(name))
        for mid in ids:
            uids = imap_suche(c, name, mid, nur_test=True)
            if uids:
                menge = b','.join(uids).decode()
                print('BEFEHL UID STORE %s +FLAGS (\\Deleted) → %s' % (menge, c.uid('STORE', menge, '+FLAGS', '(\\Deleted)')[0]))
                print('BEFEHL UID EXPUNGE %s → %s (%s)' % (menge, c.uid('EXPUNGE', menge)[0], mid))
        c.select('"%s"' % name, readonly=True)
        for uid in c.uid('SEARCH', None, 'SUBJECT', '"%s"' % betreff[len(PREFIX) + 1:])[1][0].split():
            d = c.uid('FETCH', uid, '(FLAGS BODY.PEEK[HEADER.FIELDS (SUBJECT)])')[1]
            if str(email.header.make_header(email.header.decode_header(d[0][1].decode().partition(':')[2].strip()))) != betreff:
                continue
            weg_ = '\\Deleted' in d[0][0].decode()
            print('ENTWURF %s %d %s' % (uid.decode(), weg_, betreff))
            sichtbar += not weg_
            geloescht += weg_
        c.logout()
    else:
        ordner = {graph(k, 'GET', '/me/mailFolders/' + w)['id']: w for w in ('drafts', 'deleteditems')}
        for mid in ids:
            filt = urllib.parse.quote("internetMessageId eq '%s'" % mid)
            for m in graph(k, 'GET', '/me/messages?$filter=%s&$select=id,parentFolderId,subject' % filt)['value']:
                if m['parentFolderId'] in ordner and PREFIX in (m['subject'] or ''):
                    graph(k, 'POST', '/me/messages/%s/permanentDelete' % m['id'])
                    print('BEFEHL permanentDelete …%s (%s, %s)' % (m['id'][-16:], ordner[m['parentFolderId']], mid))
        for gid, w in ordner.items():
            for m in graph(k, 'GET', '/me/mailFolders/%s/messages?$top=200&$select=id,subject' % w)['value']:
                if m['subject'] == betreff:
                    print('ENTWURF %s %d %s' % (m['id'][-16:], w != 'drafts', m['subject']))
                    sichtbar += w == 'drafts'
                    geloescht += w != 'drafts'
    print('ENTWUERFE %d %d' % (sichtbar, geloescht))
    return 0


# tests/aktionen.sh setzt je Durchgang einen eigenen Namen: microsoft365 verwechselt einen neu angelegten Ordner
# mit dem gelöschten gleichen Namens (ErrorSyncFolderNotFound, gemessen #363).
AKTION_ORDNER = os.environ.get('AKTION_ORDNER', 'Revolution-Aktionen')
# Je Durchgang eine eigene Message-ID: Kopien früherer Durchgänge in „Gelöschte Elemente“ zählen nicht mit.
AKTION_ID = msgid('aktionen-363-' + AKTION_ORDNER)
# Ziel neben dem Ordner, nicht darunter: microsoft365 behält die Unterordner eines gelöschten Ordners im Cache
# (gemessen #363, auch wenn das Kind zuerst gelöscht wird).
AKTION_ZIEL = AKTION_ORDNER + '-Ziel'


def imap_alle(c):
    """[(IMAP-Name, Camel-Name, Attribute)] aller wählbaren Ordner (LIST)."""
    out = []
    for zeile in c.list()[1]:
        m = re.match(rb'\((.*?)\) "(.*?)" "?(.*?)"?$', zeile)
        if m and b'\\Noselect' not in m.group(1):
            name = m.group(3).decode()
            out.append((name, mutf7_decode(name).replace(m.group(2).decode(), '/'), m.group(1)))
    return out


def ort(k, was, aktion=None):
    """#363: siehe Kopf (--ort). Aktions-Mail in eigenem Ordner Revolution-Aktionen (daneben „…-Ziel“ fürs
    Verschieben), nicht in Revolution-Test (dort arbeiten andere Tests mit der obersten Zeile, PM 28.10.).
    Gmail: „Alle Nachrichten“ (\\All) enthält jede Mail außer Papierkorb und Spam; zurück daraus per COPY."""
    if aktion:
        nur_testkonten([k.adresse])
    ids = {'A': AKTION_ID} if was == 'aktion' else \
        {str(nr): msgid('konversation-357-%d' % nr) for nr, eigen, _, _ in KONVERSATION if not eigen}
    gefunden = {}
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        test = imap_ordner(c)[0]
        pfad = test[:len(test) - len(ORDNER)] + AKTION_ORDNER
        trenner = c.list('""', '"%s"' % test)[1][0].decode().split('"')[1]
        if aktion == 'anlegen':
            for name in (pfad, pfad + '-Ziel'):
                if not c.list('""', '"%s"' % name)[1][0]:
                    print(c.create('"%s"' % name), c.subscribe('"%s"' % name))
        for _ in range(10):  # Gmail listet einen neuen Ordner nicht immer sofort (gemessen #363)
            alle = imap_alle(c)
            heim = next(((n, cm) for n, cm, _ in alle if n == pfad), None) if was == 'aktion' else ('INBOX', 'INBOX')
            if heim or aktion != 'anlegen':
                break
            time.sleep(2)
        for name, camel, attr in alle:
            c.select('"%s"' % name, readonly=True)
            for nr, mid in ids.items():
                for uid in c.uid('SEARCH', None, 'UNDELETED', 'HEADER', 'Message-ID', mid)[1][0].split():
                    print('ORT %s %s %d' % (nr, camel, '\\Seen' in c.uid('FETCH', uid, '(FLAGS)')[1][0].decode()))
                    gefunden.setdefault(nr, []).append((name, camel, uid, b'\\All' in attr))
        for nr, orte in gefunden.items():
            if aktion == 'zurueck' and all(o[1] != heim[1] for o in orte):
                quelle = next((o for o in orte if not o[3]), orte[0])
                c.select('"%s"' % quelle[0])
                print(c.uid('COPY' if quelle[3] else 'MOVE', quelle[2], '"%s"' % heim[0]))
        if aktion == 'anlegen' and 'A' not in gefunden:
            m = EmailMessage(policy=email.policy.SMTP)
            m.set_content('Testmail für Löschen, Archiv, Spam, Verschieben (#363).\n')
            m['Subject'], m['From'], m['To'] = PREFIX + ' Aktionen 363', ANZEIGE_ABSENDER, k.adresse
            m['Date'], m['Message-ID'] = email.utils.formatdate(localtime=True), AKTION_ID
            print(c.append('"%s"' % heim[0], None, None, m.as_bytes()))
        if aktion == 'aufraeumen' and was == 'aktion':
            # Nur die eigene Mail (Message-ID) endgültig: \\Deleted + UID EXPUNGE ihrer UIDs; bei Gmail aus
            # „Alle Nachrichten“ erst in den Papierkorb (Expunge dort nähme nur das Label).
            papierkorb = next((n for n, _, at in alle if b'\\Trash' in at), None)
            for name, _, uid, alle_nachrichten in sorted(gefunden.get('A', []), key=lambda o: o[3]):
                c.select('"%s"' % name)
                if alle_nachrichten and papierkorb:
                    print(c.uid('MOVE', uid, '"%s"' % papierkorb))
                    continue
                print(c.uid('STORE', uid, '+FLAGS', '(\\Deleted)'), c.uid('EXPUNGE', uid))
            if papierkorb:
                c.select('"%s"' % papierkorb)
                for uid in c.uid('SEARCH', None, 'HEADER', 'Message-ID', AKTION_ID)[1][0].split():
                    print(c.uid('STORE', uid, '+FLAGS', '(\\Deleted)'), c.uid('EXPUNGE', uid))
            c.select('INBOX', readonly=True)
            for name in (pfad + '-Ziel', pfad):
                print(c.delete('"%s"' % name))
        if heim:
            print('HEIM %s' % heim[1])
        c.logout()
        return 0
    # Angelegte Ordner-IDs in AKTION_IDS (Datei des Laufs); endgültig gelöscht werden nur diese (PM 28.10.).
    ids_datei = os.environ.get('AKTION_IDS')
    eigene = graph(k, 'GET', "/me/mailFolders?$filter=displayName%20eq%20'" + AKTION_ORDNER + "'")['value']
    if aktion == 'anlegen' and not eigene:
        eigene = [graph(k, 'POST', '/me/mailFolders', {'displayName': AKTION_ORDNER})]
        ziel = graph(k, 'POST', '/me/mailFolders', {'displayName': AKTION_ZIEL})
        with open(ids_datei, 'a') as f:
            f.write('%s %s\n%s %s\n' % (eigene[0]['id'], AKTION_ORDNER, ziel['id'], AKTION_ZIEL))
        print('ANGELEGT %s %s, %s %s' % (eigene[0]['id'], AKTION_ORDNER, ziel['id'], AKTION_ZIEL))
    heim = (eigene[0]['id'] if eigene else None) if was == 'aktion' else graph(k, 'GET', '/me/mailFolders/inbox')['id']
    for nr, mid in ids.items():
        mails = graph(k, 'GET', "/me/messages?$filter=internetMessageId%20eq%20'" + urllib.parse.quote(mid)
                      + "'&$select=isRead,parentFolderId")['value']
        for m in mails:
            ordner = graph(k, 'GET', '/me/mailFolders/%s?$select=displayName' % m['parentFolderId'])['displayName']
            print('ORT %s %s %d' % (nr, ordner, m['isRead']))
            gefunden[nr] = True
            # Nur eine Kopie und nur, wenn keine mehr daheim ist (ältere Kopien im Papierkorb bleiben).
            if aktion == 'zurueck' and all(x['parentFolderId'] != heim for x in mails) and m is mails[0]:
                print(graph(k, 'POST', '/me/messages/%s/move' % m['id'], {'destinationId': heim})['parentFolderId'] == heim)
            if aktion == 'aufraeumen' and was == 'aktion' and ordner != 'Gelöschte Elemente':
                graph(k, 'POST', '/me/messages/%s/move' % m['id'], {'destinationId': 'deleteditems'})
                print('IN GELÖSCHTE ELEMENTE aus %s' % ordner)
    if aktion == 'anlegen' and 'A' not in gefunden:
        # PR_MESSAGE_FLAGS 0 (ungelesen, nicht MSGFLAG_UNSENT): keine Entwurf-Mail.
        graph(k, 'POST', '/me/mailFolders/%s/messages' % heim, {
            'subject': PREFIX + ' Aktionen 363', 'internetMessageId': AKTION_ID,
            'body': {'contentType': 'text', 'content': 'Testmail für Löschen, Archiv, Spam, Verschieben (#363).'},
            'from': {'emailAddress': {'address': ANZEIGE_ABSENDER}}, 'toRecipients': [{'emailAddress': {'address': k.adresse}}],
            'singleValueExtendedProperties': [{'id': 'Integer 0x0E07', 'value': '0'}]})
    if aktion == 'aufraeumen' and was == 'aktion' and ids_datei and os.path.exists(ids_datei):
        for zeile in reversed(open(ids_datei).read().split('\n')):
            if zeile.endswith(' ' + AKTION_ORDNER) or zeile.endswith(' ' + AKTION_ZIEL):
                fid = zeile.split(' ', 1)[0]
                graph(k, 'POST', '/me/mailFolders/%s/permanentDelete' % fid)
                print('ORDNER GELOESCHT %s' % zeile)
        os.rename(ids_datei, ids_datei + '.erledigt')
    if eigene or was != 'aktion':
        print('HEIM %s' % (AKTION_ORDNER if was == 'aktion' else 'Posteingang'))
    return 0


TESTORDNER = 'Revolution-Offline'


def testordner(k, aktion):
    """#408: Ordner Revolution-Offline neben Revolution-Test am Server anlegen oder löschen (IMAP
    CREATE/DELETE, nur Testkonten), während die App offline ist; Zeile „TESTORDNER <Camel-Name>“."""
    nur_testkonten([k.adresse])
    if not k.goa.get_mail().props.imap_supported:
        sys.exit('ABBRUCH: --testordner nur für IMAP')
    c = imap_verbinden(k)
    name, camel_name = imap_ordner(c)
    name = name[:len(name) - len(ORDNER)] + TESTORDNER
    camel_name = camel_name[:len(camel_name) - len(ORDNER)] + TESTORDNER
    print(c.create('"%s"' % name) if aktion == 'anlegen' else c.delete('"%s"' % name))
    print('TESTORDNER %s' % camel_name)
    c.logout()
    return 0


# #412: Wechsel weg von einer großen Mail. Drei Mails per APPEND in den Posteingang (nicht privat@,
# Quota): Start (blau), Schwer (≥ 500 KB, 8000 Tabellenzeilen, gelb), Ziel (grün, ungelesen). Datum
# jetzt bzw. 1 und 2 Minuten davor (nie in der Zukunft): nach Datum absteigend stehen sie in dieser
# Folge untereinander. Flächenfarben, damit der Treiber jedes Bild der Anzeige zuordnen kann.
SCHWER = (('Start', '#3465a4', True), ('Schwer', '#f5c211', True), ('Ziel', '#2ec27e', False))


def schwer_html(rolle, farbe):
    if rolle == 'Schwer':
        zeilen = ''.join('<tr style="background:%s"><td>Zeile %d</td><td>Testinhalt der großen Mail #412</td>'
                         '<td>%d</td></tr>' % (farbe, i, i * 7) for i in range(8000))
        return '<html><body><table width="100%%">%s</table></body></html>\n' % zeilen
    return ('<html><body><div style="background:%s;height:2000px">%s (#412)</div></body></html>\n'
            % (farbe, rolle))


def schwer(k, aktion):
    """#412: neu (APPEND, idempotent) oder papierkorb (UID MOVE in den Ordner mit \\Trash, nicht
    endgültig gelöscht). Zeilen „ORDNER <Konto-UID>/INBOX“, „SCHWER <Rolle> <UID> <Bytes>“."""
    nur_testkonten([k.adresse])
    if k.adresse == KONTO_IMAP_1:
        sys.exit('ABBRUCH: --schwer nicht in privat@ (Quota, #411)')
    if not k.goa.get_mail().props.imap_supported:
        sys.exit('ABBRUCH: --schwer nur für IMAP')
    c = imap_verbinden(k)
    c.select('INBOX')

    def vorhanden():
        out = {}
        for rolle, _, _ in SCHWER:
            _, daten = c.uid('SEARCH', None, 'SUBJECT', '"Pruef412 %s"' % rolle)
            uids = daten[0].split()
            if uids:
                _, d = c.uid('FETCH', uids[-1], '(RFC822.SIZE)')
                out[rolle] = (uids[-1].decode(), re.search(rb'RFC822\.SIZE (\d+)', d[0]).group(1).decode())
        return out

    if aktion == 'neu':
        jetzt = time.time()
        da = vorhanden()
        for n, (rolle, farbe, gelesen) in enumerate(SCHWER):
            if rolle in da:
                # Ausgangszustand eines früheren Laufs: Ziel ungelesen.
                if not gelesen:
                    print(c.uid('STORE', da[rolle][0], '-FLAGS', '(\\Seen)'))
                continue
            m = EmailMessage(policy=email.policy.SMTP)
            m.set_content('%s (#412), Inhalt nur in der HTML-Fassung.\n' % rolle)
            m.add_alternative(schwer_html(rolle, farbe), subtype='html')
            m['Subject'] = '%s Pruef412 %s' % (PREFIX, rolle)
            m['From'] = email.utils.formataddr(('Revolution Testdaten', k.adresse))
            m['To'] = k.adresse
            m['Date'] = email.utils.formatdate(jetzt - 60 * n, localtime=True)
            print(c.append('INBOX', '(\\Seen)' if gelesen else None, imaplib.Time2Internaldate(jetzt - 60 * n),
                           m.as_bytes()))
    else:
        papierkorb = next(re.match(rb'.*" "?(.*?)"?$', z).group(1).decode() for z in c.list()[1]
                          if b'\\Trash' in z)
        for rolle, _, _ in SCHWER:
            _, daten = c.uid('SEARCH', None, 'SUBJECT', '"Pruef412 %s"' % rolle)
            for uid in daten[0].split():
                print(rolle, uid.decode(), c.uid('MOVE', uid, '"%s"' % papierkorb))
        c.logout()
        return 0
    print('ORDNER %s/INBOX' % k.account.get_uid())
    for rolle, (uid, groesse) in vorhanden().items():
        print('SCHWER %s %s %s' % (rolle, uid, groesse))
    c.logout()
    return 0


# ---------------------------------------------------------------- #350: Ungelesen-Zähler

def mutf7_decode(name):
    """IMAP-Ordnername (modifiziertes UTF-7, RFC 3501 5.1.3) → Text."""
    def teil(m):
        if m.group(1) == '':
            return '&'
        b = m.group(1).replace(',', '/')
        return base64.b64decode(b + '=' * (-len(b) % 4)).decode('utf-16-be')
    return re.sub(r'&([^-]*)-', teil, name)


def zaehler_imap(k):
    """{voller Name wie Camel: n} per LIST und STATUS (UNSEEN), nur lesend (kein SELECT/EXAMINE)."""
    c = imap_verbinden(k)
    try:
        out = {}
        typ, zeilen = c.list('""', '*')
        for z in zeilen:
            if not isinstance(z, bytes):
                continue
            m = re.match(rb'\((?P<flags>[^)]*)\) (?P<delim>"[^"]*"|NIL) (?P<name>.*)$', z)
            if not m or b'\\noselect' in m.group('flags').lower() or b'\\nonexistent' in m.group('flags').lower():
                continue
            roh = m.group('name').decode()
            delim = m.group('delim').decode().strip('"')
            typ, st = c.status(roh if roh.startswith('"') else '"%s"' % roh, '(UNSEEN)')
            n = int(re.search(rb'UNSEEN (\d+)', st[0]).group(1))
            name = mutf7_decode(roh.strip('"'))
            out[name.replace(delim, '/') if delim != 'NIL' else name] = n
        return out
    finally:
        c.logout()


def anzahl(konten, uid, ziel):
    """#410: nur lesend, unabhängig von Camel: Nachrichten eines IMAP-Ordners per LIST, EXAMINE
    (EXISTS) und UID SEARCH; „sichtbar“ ohne \\Deleted und Junk wie die Liste der App
    (mail-view.c hidden_flags). Zeile „ANZAHL <UID>/<Ordner> <sichtbar> EXISTS <n>“."""
    k = next((k for k in konten if k.account.get_uid() == uid), None)
    if k is None:
        sys.exit('ABBRUCH: Konto %s nicht in GOA/EDS' % uid)
    if k.account.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_backend_name() == 'microsoft365':
        # #414: Graph zählt alle Nachrichten des Ordners (totalItemCount, nur GET).
        ref = zaehler_graph(k, 'totalItemCount')
        if ziel not in ref:
            sys.exit('ABBRUCH: Ordner %s fehlt' % ziel)
        print('ANZAHL %s/%s %d EXISTS %d' % (uid, ziel, ref[ziel], ref[ziel]))
        return 0
    c = imap_verbinden(k)
    try:
        roh = None
        for z in c.list('""', '*')[1]:
            m = re.match(rb'\((?P<flags>[^)]*)\) (?P<delim>"[^"]*"|NIL) (?P<name>.*)$', z) \
                if isinstance(z, bytes) else None
            if not m:
                continue
            delim = m.group('delim').decode().strip('"')
            name = mutf7_decode(m.group('name').decode().strip('"'))
            if (name.replace(delim, '/') if delim != 'NIL' else name) == ziel:
                roh = m.group('name').decode()
        if roh is None:
            sys.exit('ABBRUCH: Ordner %s fehlt' % ziel)
        typ, d = c.select(roh if roh.startswith('"') else '"%s"' % roh, readonly=True)
        exists = int(d[0])
        sichtbar = len(c.uid('SEARCH', None, 'UNDELETED', 'UNKEYWORD', 'Junk', 'UNKEYWORD', '$Junk')[1][0].split())
    finally:
        c.logout()
    print('ANZAHL %s/%s %d EXISTS %d' % (uid, ziel, sichtbar, exists))
    return 0


def zaehler_graph(k, feld='unreadItemCount'):
    """{Pfad der Anzeigenamen wie Camel microsoft365: n} per Graph, nur GET; #414: feld
    totalItemCount für --anzahl."""
    q = '?' + urllib.parse.urlencode({'$select': 'displayName,childFolderCount,' + feld, '$top': '100'})
    out = {}

    def holen(pfad, prefix):
        while pfad:
            d = graph(k, 'GET', pfad)
            for f in d['value']:
                name = prefix + f['displayName']
                out[name] = f[feld]
                if f.get('childFolderCount'):
                    holen('/me/mailFolders/%s/childFolders%s' % (f['id'], q), name + '/')
            pfad = d.get('@odata.nextLink', '').removeprefix(GRAPH) or None

    holen('/me/mailFolders' + q, '')
    return out


ORDNERTEST = 'Revolution-Ordnertest'


def ordnertest(k):
    """#364: nur lesend. Testordner am Server, auch \\Noselect (verwaiste Elternordner)."""
    if k.account.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_backend_name() == 'microsoft365':
        namen = list(zaehler_graph(k, 'totalItemCount'))
    else:
        c = imap_verbinden(k)
        namen = []
        for z in c.list('""', '*')[1]:
            m = re.match(rb'\((?P<flags>[^)]*)\) (?P<delim>"[^"]*"|NIL) (?P<name>.*)$', z) \
                if isinstance(z, bytes) else None
            if m and b'\\nonexistent' not in m.group('flags').lower():
                delim = m.group('delim').decode().strip('"')
                name = mutf7_decode(m.group('name').decode().strip('"'))
                namen.append(name.replace(delim, '/') if delim != 'NIL' else name)
        c.logout()
    treffer = sorted(n for n in namen if any(t.startswith(ORDNERTEST) for t in n.split('/')))
    for n in treffer:
        print('ORDNERTEST %s' % n)
    print('ORDNERTEST-ANZAHL %d' % len(treffer))
    return 0


def zaehler(konten, log):
    """#350: nur lesend. Ungelesene je Ordner unabhängig von Camel; mit log Vergleich mit dem
    letzten „Ungelesen: <UID> <Ordner> <N>“ der App je Ordner."""
    app = {}
    if log:
        for z in open(log, encoding='utf-8', errors='replace'):
            m = re.search(r'revolution-DEBUG: [0-9:.]+: Ungelesen: (\S+) (.+) (\d+)$', z.rstrip('\n'))
            if m:
                app[(m.group(1), m.group(2))] = int(m.group(3))
    fehler = gleich = ohne = 0
    for k in konten:
        uid = k.account.get_uid()
        backend = k.account.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_backend_name()
        ref = zaehler_graph(k) if backend == 'microsoft365' else zaehler_imap(k)
        for name in sorted(ref):
            print('ZAEHLER %s %s %d' % (uid, name, ref[name]))
            if not log:
                continue
            if (uid, name) not in app:
                # App ohne Zahl: imapx-Ordner ohne Abgleich in dieser Sitzung (main.c forget_unread)
                print('  OHNE ZAHL in der App: %s %s (Server %d)' % (k.adresse, name, ref[name]))
                ohne += 1
            elif app[(uid, name)] != ref[name]:
                print('  ABWEICHUNG %s %s: App %d, Server %d' % (k.adresse, name, app[(uid, name)], ref[name]))
                fehler += 1
            else:
                gleich += 1
    if log:
        print('Vergleich: %d gleich, %d abweichend, %d ohne Zahl in der App' % (gleich, fehler, ohne))
    return 1 if fehler else 0


# ---------------------------------------------------------------- #351: Kombinierter Eingang

FAVORITEN = 'revolution-favoriten'
SICHTBAR = ('UNDELETED', 'UNKEYWORD', 'Junk', 'UNKEYWORD', '$Junk')


def posteingang(k):
    """(sichtbar, ungelesen) im Posteingang, nur lesend: IMAP EXAMINE + UID SEARCH ohne Gelöschte und
    Junk wie die Liste der App (mail-favorites.c VISIBLE), microsoft365 Graph GET."""
    backend = k.account.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_backend_name()
    if backend == 'microsoft365':
        d = graph(k, 'GET', '/me/mailFolders/inbox?$select=totalItemCount,unreadItemCount')
        return d['totalItemCount'], d['unreadItemCount']
    c = imap_verbinden(k)
    try:
        c.select('INBOX', readonly=True)
        n = len(c.uid('SEARCH', None, *SICHTBAR)[1][0].split())
        u = len(c.uid('SEARCH', None, 'UNSEEN', *SICHTBAR)[1][0].split())
    finally:
        c.logout()
    return n, u


def kombiniert(konten, log):
    """#351: nur lesend. Je Konto Posteingang vom Server; mit log Vergleich mit der App."""
    app, liste, fav_u = {}, None, None
    if log:
        for z in open(log, encoding='utf-8', errors='replace'):
            m = re.search(r'revolution-DEBUG: [0-9:.]+: (.*)$', z.rstrip('\n'))
            if not m:
                continue
            t = m.group(1)
            a = re.match(r'Favoriten: Anzahl inbox (\S+) (.+) (\d+)$', t)
            if a:
                app[a.group(1)] = int(a.group(3))
            a = re.match(r'Nachrichten: %s inbox (?:Cache |Server |vollständig |.*Liste )(\d+)' % FAVORITEN, t)
            if a:
                liste = int(a.group(1))
            a = re.match(r'Ungelesen: %s inbox (\d+)$' % FAVORITEN, t)
            if a:
                fav_u = int(a.group(1))
    fehler = 0
    summe_n = summe_u = 0
    for k in konten:
        uid = k.account.get_uid()
        n, u = posteingang(k)
        summe_n += n
        summe_u += u
        print('KOMBINIERT %s %s %d %d' % (uid, k.adresse, n, u))
        if log:
            if app.get(uid) != n:
                print('  ABWEICHUNG %s: App %s, Server %d' % (k.adresse, app.get(uid), n))
                fehler += 1
            else:
                print('  GLEICH %s: App %d = Server %d' % (k.adresse, n, n))
    print('SUMME %d Konten, %d sichtbar, %d ungelesen' % (len(konten), summe_n, summe_u))
    if log:
        for was, ist, soll in (('Liste „Alle Eingänge“', liste, summe_n), ('Ungelesen „Alle Eingänge“', fav_u, summe_u)):
            print('  %s %s: App %s, Server %d' % ('GLEICH' if ist == soll else 'ABWEICHUNG', was, ist, soll))
            fehler += ist != soll
        if len(app) != len(konten):
            print('  ABWEICHUNG Konten in „Alle Eingänge“: App %d, GOA %d' % (len(app), len(konten)))
            fehler += 1
        print('Vergleich: %d Abweichungen' % fehler)
    return 1 if fehler else 0


def live_flags(k):
    """#351: nur lesend (EXAMINE INBOX)."""
    c = imap_verbinden(k)
    try:
        c.select('INBOX', readonly=True)
        for uid in c.uid('SEARCH', None, 'SUBJECT', '"%s"' % LIVE)[1][0].split():
            flags = c.uid('FETCH', uid, '(FLAGS)')[1][0].decode()
            print('FLAGS %d %d %s' % ('\\Seen' in flags, '\\Flagged' in flags, uid.decode()))
    finally:
        c.logout()
    return 0


# ---------------------------------------------------------------- #362 Senden

def imap_rollen(c):
    """{rolle: IMAP-Name} für eingang, spam, gesendet, papierkorb (SPECIAL-USE, sonst Name)."""
    rollen = {'eingang': 'INBOX'}
    for zeile in c.list()[1]:
        treffer = re.match(rb'\((.*?)\) "(.*?)" "?(.*?)"?$', zeile)
        if not treffer:
            continue
        attr, name = treffer.group(1).decode(), treffer.group(3).decode()
        kurz = name.split(treffer.group(2).decode())[-1]
        if '\\Sent' in attr:
            rollen['gesendet'] = name
        elif '\\Junk' in attr or ('spam' not in rollen and kurz in ('Junk', 'Spam')):
            rollen['spam'] = name
        elif '\\Trash' in attr:
            rollen['papierkorb'] = name
    return rollen


def imap_suche(c, name, message_id, nur_test=False):
    c.select('"%s"' % name, readonly=not nur_test)
    uids = c.uid('SEARCH', None, 'HEADER', 'Message-ID', '"%s"' % message_id)[1][0].split()
    if nur_test:
        uids = [u for u in uids if PREFIX.encode() in c.uid('FETCH', u, '(BODY.PEEK[HEADER.FIELDS (SUBJECT)])')[1][0][1]]
    return uids


GRAPH_ROLLEN = (('eingang', 'inbox'), ('spam', 'junkemail'), ('gesendet', 'sentitems'))


def graph_suche(k, message_id):
    """[(rolle, id, Betreff)] der Nachrichten mit dieser internetMessageId, nur GET."""
    ordner = {graph(k, 'GET', '/me/mailFolders/' + w)['id']: r for r, w in GRAPH_ROLLEN}
    filt = urllib.parse.quote("internetMessageId eq '%s'" % message_id)
    mails = graph(k, 'GET', '/me/messages?$filter=%s&$select=parentFolderId,subject' % filt)['value']
    return [(ordner.get(m['parentFolderId'], 'andere'), m['id'], m['subject']) for m in mails]


def zustellung(k, message_id):
    """#362: nur lesend."""
    if not message_id.startswith('<'):
        message_id = '<%s>' % message_id
    zahl = {'eingang': 0, 'spam': 0, 'gesendet': 0}
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        try:
            for rolle, name in imap_rollen(c).items():
                if rolle in zahl:
                    zahl[rolle] = len(imap_suche(c, name, message_id))
        finally:
            c.logout()
    else:
        for rolle, _, _ in graph_suche(k, message_id):
            if rolle in zahl:
                zahl[rolle] += 1
    for rolle, n in zahl.items():
        print('ZUSTELLUNG %s %s %d' % (k.adresse, rolle, n))
    return 0


def aufraeumen(k, message_ids):
    """#362: nur Testkonten, nur [Revolution-Test]-Mails mit diesen Message-IDs."""
    nur_testkonten([k.adresse])
    ids = [m if m.startswith('<') else '<%s>' % m for m in message_ids]
    if k.goa.get_mail().props.imap_supported:
        c = imap_verbinden(k)
        try:
            rollen = imap_rollen(c)
            gmail = 'gmail' in k.adresse
            for rolle in ('eingang', 'spam', 'gesendet'):
                if rolle not in rollen:
                    continue
                n = 0
                for mid in ids:
                    uids = imap_suche(c, rollen[rolle], mid, nur_test=True)
                    if not uids:
                        continue
                    n += len(uids)
                    menge = b','.join(uids).decode()
                    if gmail:
                        c.uid('MOVE', menge, '"%s"' % rollen['papierkorb'])
                    else:
                        c.uid('STORE', menge, '+FLAGS', '(\\Deleted)')
                        c.uid('EXPUNGE', menge)
                print('AUFGERAEUMT %s %s %d' % (k.adresse, rolle, n))
            if gmail:
                n = 0
                for mid in ids:
                    uids = imap_suche(c, rollen['papierkorb'], mid, nur_test=True)
                    if uids:
                        n += len(uids)
                        menge = b','.join(uids).decode()
                        c.uid('STORE', menge, '+FLAGS', '(\\Deleted)')
                        c.uid('EXPUNGE', menge)
                print('AUFGERAEUMT %s papierkorb %d' % (k.adresse, n))
        finally:
            c.logout()
    else:
        zahl = {}
        for mid in ids:
            for rolle, gid, betreff in graph_suche(k, mid):
                # #432: Antworten/Weiterleitungen tragen „Re:“/„Fwd:“ vor dem Präfix.
                if PREFIX not in (betreff or ''):
                    continue
                graph(k, 'POST', '/me/messages/%s/permanentDelete' % gid)
                zahl[rolle] = zahl.get(rolle, 0) + 1
        for rolle in ('eingang', 'spam', 'gesendet'):
            print('AUFGERAEUMT %s %s %d' % (k.adresse, rolle, zahl.get(rolle, 0)))
    return 0


# #432: Originale für Antworten, Allen antworten, Weiterleiten aus dem Composer, je Lauf eigene Message-IDs,
# ungelesen im Posteingang. Absender und Empfänger nur Testkonten: in geops@ von privat@ an geops@, Kopie outlook.com;
# in outlook.com von geops@ an outlook.com, Kopie privat@.
ORIGINAL_ARTEN = ('Antworten', 'Allen', 'Weiterleiten')


def original_daten(k, lauf, art):
    von, kopie = ((KONTO_IMAP_1, KONTO_M365) if k.adresse == KONTO_IMAP_2
                  else (KONTO_IMAP_2, KONTO_IMAP_1))
    nur_testkonten([von, kopie, k.adresse])
    return von, kopie, '%s 432 %s %s' % (PREFIX, lauf, art), msgid('original-432-%s-%s' % (lauf, art.lower()))


def original(k, lauf, aktion):
    """anlegen: drei Originale; flags: nur lesend Flags vom Server; aufraeumen: genau diese (UID EXPUNGE bzw.
    permanentDelete der gefundenen IDs)."""
    nur_testkonten([k.adresse])
    imap = k.goa.get_mail().props.imap_supported
    c = imap_verbinden(k) if imap else None
    try:
        for art in ORIGINAL_ARTEN:
            von, kopie, betreff, mid = original_daten(k, lauf, art)
            if imap:
                c.select('INBOX', readonly=aktion == 'flags')
                uids = imap_suche(c, 'INBOX', mid, nur_test=aktion != 'flags')
            else:
                uids = [g for r, g, b in graph_suche(k, mid) if PREFIX in (b or '')]
            if aktion == 'anlegen' and not uids:
                if imap:
                    m = EmailMessage(policy=email.policy.SMTP)
                    m.set_content('Original für #432 (%s).\n' % art)
                    m['Subject'], m['From'], m['To'], m['Cc'] = betreff, von, k.adresse, kopie
                    m['Date'] = email.utils.format_datetime(datetime.datetime.now(datetime.timezone.utc)
                                                            - datetime.timedelta(minutes=1))
                    m['Message-ID'] = mid
                    print(c.append('INBOX', '()', None, m.as_bytes()))
                    uids = imap_suche(c, 'INBOX', mid)
                else:
                    # PR_MESSAGE_FLAGS 0 (ungelesen, kein Entwurf) wie ort().
                    uids = [graph(k, 'POST', '/me/mailFolders/inbox/messages', {
                        'subject': betreff, 'internetMessageId': mid,
                        'body': {'contentType': 'text', 'content': 'Original für #432 (%s).' % art},
                        'from': {'emailAddress': {'address': von}},
                        'toRecipients': [{'emailAddress': {'address': k.adresse}}],
                        'ccRecipients': [{'emailAddress': {'address': kopie}}],
                        'singleValueExtendedProperties': [{'id': 'Integer 0x0E07', 'value': '0'}]})['id']]
                print('ORIGINAL %s %s %s %s' % (art, mid, uids[0].decode() if imap else uids[0], betreff))
            elif aktion == 'flags':
                for u in uids:
                    if imap:
                        print('FLAGS %s %s' % (art, re.search(r'FLAGS \((.*?)\)', c.uid('FETCH', u, '(FLAGS)')[1][0].decode()).group(1)))
                    else:
                        # PidTagLastVerbExecuted (0x1081): Exchange setzt es bei Antwort/Weiterleiten über seine API.
                        m = graph(k, 'GET', "/me/messages/%s?$select=isRead&$expand=singleValueExtendedProperties"
                                  "($filter=id%%20eq%%20'Integer%%200x1081')" % u)
                        verb = [p['value'] for p in m.get('singleValueExtendedProperties', [])]
                        print('FLAGS %s isRead=%s LastVerbExecuted=%s' % (art, m['isRead'], verb[0] if verb else '-'))
                if not uids:
                    print('FLAGS %s fehlt' % art)
            elif aktion == 'aufraeumen' and uids:
                if imap:
                    menge = b','.join(uids).decode()
                    print('BEFEHL UID STORE %s +FLAGS (\\Deleted) → %s' % (menge, c.uid('STORE', menge, '+FLAGS', '(\\Deleted)')[0]))
                    print('BEFEHL UID EXPUNGE %s → %s (%s)' % (menge, c.uid('EXPUNGE', menge)[0], mid))
                else:
                    for g in uids:
                        graph(k, 'POST', '/me/messages/%s/permanentDelete' % g)
                        print('BEFEHL permanentDelete …%s (%s)' % (g[-16:], mid))
    finally:
        if c is not None:
            c.logout()
    return 0


def main():
    ap = argparse.ArgumentParser(description='Testdaten für Revolution (#344)')
    ap.add_argument('--dry-run', action='store_true', help='nur anzeigen, nichts senden/schreiben')
    ap.add_argument('--pruefen', action='store_true', help='nur lesend prüfen')
    ap.add_argument('--ordner', action='store_true', help='nur lesend: Ordnerliste aller Konten')
    ap.add_argument('--anzeige', action='store_true',
                    help='#354: nur die Anzeige-Mails per APPEND in die Posteingänge')
    ap.add_argument('--anhang', action='store_true',
                    help='#356: nur die Anhang-Mail per APPEND (geops@, outlook.com, Gmail)')
    ap.add_argument('--anhang-teile', nargs=2, metavar=('KONTO', 'ORDNER'),
                    help='#356: nur lesend die Anhänge der Anhang-Mail vom Server nach ORDNER')
    ap.add_argument('--last', type=int, metavar='N',
                    help='N Lastmails per APPEND in %s von %s (#352)' % (LAST_ORDNER, LAST_KONTO))
    ap.add_argument('--last-ordner', default=LAST_ORDNER, metavar='NAME',
                    help='#350: Ordner für --last statt %s (z. B. Revolution-Zaehler)' % LAST_ORDNER)
    ap.add_argument('--flags', metavar='KONTO', help='#353: nur lesend Flags in %s vom Server' % ORDNER)
    ap.add_argument('--ungelesen', nargs=2, metavar=('KONTO', 'ORDNER'),
                    help='#353: ungelesene UIDs (nur lesend); mit --soll DATEI wiederherstellen')
    ap.add_argument('--zaehler', nargs='?', const='', metavar='LOG',
                    help='#350: nur lesend Ungelesene je Ordner (IMAP STATUS, Graph), Vergleich mit LOG')
    ap.add_argument('--anzahl', nargs=2, metavar=('UID', 'ORDNER'),
                    help='#410: nur lesend Nachrichten eines Ordners (IMAP EXAMINE, UID SEARCH; #414 Graph)')
    ap.add_argument('--warte', metavar='DATEI', help='#414: --server erst ausführen, wenn DATEI existiert')
    ap.add_argument('--soll', metavar='DATEI', help='#353: Ausgabe von --ungelesen als Sollstand')
    ap.add_argument('--server', nargs='+', metavar='KONTO AKTION [INBOX|Papierkorb]',
                    help='#353: serverseitig gelesen|ungelesen|markiert|unmarkiert|neu|weg [im Posteingang]; '
                         '#416: Graph auch an der Einladung im Papierkorb')
    ap.add_argument('--kombiniert', nargs='?', const='', metavar='LOG',
                    help='#351: nur lesend Posteingänge aller Konten (EXAMINE, Graph), Vergleich mit LOG')
    ap.add_argument('--benachrichtigung', nargs=3, metavar=('KONTO', 'neu|gelesen|weg', 'NAMEN|UID|DATEI'),
                    help='#366: Testmails im Posteingang anlegen bzw. genau diese UIDs löschen')
    ap.add_argument('--entwurf', nargs='+', metavar='KONTO BETREFF [weg MESSAGE-ID …]',
                    help='#360: Entwürfe mit diesem Betreff am Server; weg: nur diese Message-IDs löschen')
    ap.add_argument('--ort', nargs='+', metavar='KONTO aktion|konversation [anlegen|zurueck|aufraeumen]',
                    help='#363: Ordner der Aktions-Mail bzw. Konversation am Server; anlegen bzw. zurück')
    ap.add_argument('--kontakt', nargs='?', const='', choices=('', 'anlegen', 'weg'),
                    help='#361: Testkontakt im Google-Adressbuch von %s' % KONTAKT_KONTO)
    ap.add_argument('--adressbuecher', action='store_true', help='#368: nur lesend Adressbücher und Kontaktzahl')
    ap.add_argument('--kontakt-live', nargs=3, metavar=('anlegen|aendern|loeschen|pruefen|refresh', 'BUCH', 'DATEI'),
                    help='#368: Testkontakt von außen über EDS')
    ap.add_argument('--kontakt-graph', nargs=2, metavar=('anlegen|loeschen', 'DATEI'),
                    help='#368: Testkontakt in %s direkt über Graph' % KONTO_M365)
    ap.add_argument('--live-flags', metavar='KONTO', help='#351: nur lesend Flags der Live-Mails im Posteingang')
    ap.add_argument('--antwort', action='store_true',
                    help='#359: Testmail für Antworten per APPEND in den Posteingang von %s' % ANTWORT_KONTO)
    ap.add_argument('--antwort-kopf', action='store_true', help='#359: nur lesend Kopfzeilen der Testmail')
    ap.add_argument('--testordner', nargs=2, metavar=('KONTO', 'anlegen|loeschen'),
                    help='#408: Ordner %s am Server anlegen oder löschen (IMAP, Testkonten)' % TESTORDNER)
    ap.add_argument('--konversation-flags', metavar='KONTO',
                    help='#357: nur lesend Flags der Konversation vom Server')
    ap.add_argument('--sortierung', nargs=2, metavar=('KONTO', 'basis|neu|papierkorb'),
                    help='#418: Faden, Antwort und neue Konversation im Posteingang (IMAP, Testkonten)')
    ap.add_argument('--konversation', action='store_true',
                    help='#357: Konversation (5 Mails) per APPEND in Posteingang und Gesendet (geops@, outlook.com)')
    ap.add_argument('--schwer', nargs=2, metavar=('KONTO', 'neu|papierkorb'),
                    help='#412: große Mail mit Start- und Zielmail im Posteingang (IMAP, nicht privat@)')
    ap.add_argument('--original', nargs=3, metavar=('KONTO', 'LAUF', 'anlegen|flags|aufraeumen'),
                    help='#432: Originale für Antworten/Allen/Weiterleiten im Posteingang (nur geops@, outlook.com)')
    ap.add_argument('--zustellung', nargs=2, metavar=('KONTO', 'MESSAGE-ID'),
                    help='#362: nur lesend Anzahl der Nachricht in Posteingang, Spam, Gesendet')
    ap.add_argument('--aufraeumen', nargs='+', metavar='KONTO MESSAGE-ID',
                    help='#362: vom Test gesendete Mails endgültig löschen (Testkonten)')
    ap.add_argument('--suche', action='store_true',
                    help='#365: Such-Mail per APPEND in geops@ (Posteingang, Revolution-Test) und outlook.com')
    ap.add_argument('--konten', action='store_true', help='#364: nur lesend UID und Adresse aller Mail-Konten')
    ap.add_argument('--ordnertest', metavar='KONTO', help='#364: nur lesend Testordner %s* vom Server' % ORDNERTEST)
    ap.add_argument('--suche-zahlen', nargs=2, metavar=('KONTO', 'ORDNER'),
                    help='#365: nur lesend Treffer der Suche und der Filter je Ordner (ORDNER * = alle per LIST, nur Text)')
    args = ap.parse_args()
    if args.original:
        adresse = args.original[0].lower()
        if adresse not in (KONTO_IMAP_2, KONTO_M365) or \
                args.original[2] not in ('anlegen', 'flags', 'aufraeumen'):
            ap.error('--original %s|%s LAUF anlegen|flags|aufraeumen' % (KONTO_IMAP_2, KONTO_M365))
        return original(konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse], *args.original[1:])
    if args.zustellung or args.aufraeumen:
        adresse = (args.zustellung or args.aufraeumen)[0].lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        if args.zustellung:
            return zustellung(k, args.zustellung[1])
        if len(args.aufraeumen) < 2:
            ap.error('--aufraeumen KONTO MESSAGE-ID …')
        return aufraeumen(k, args.aufraeumen[1:])
    if args.last is not None and args.last < 1:
        ap.error('--last N mit N ≥ 1 (#411)')

    if args.konten:
        for k in ordner_konten(EDataServer.SourceRegistry.new_sync(None)):
            print('KONTO %s %s' % (k.account.get_uid(), k.adresse))
        return 0
    if args.ordnertest:
        adresse = args.ordnertest.lower()
        nur_testkonten([adresse])
        return ordnertest(konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse])

    if args.testordner:
        adresse = args.testordner[0].lower()
        nur_testkonten([adresse])
        if args.testordner[1] not in ('anlegen', 'loeschen'):
            ap.error('--testordner KONTO anlegen|loeschen')
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        return testordner(k, args.testordner[1])

    if args.schwer:
        adresse = args.schwer[0].lower()
        nur_testkonten([adresse])
        if args.schwer[1] not in ('neu', 'papierkorb'):
            ap.error('--schwer KONTO neu|papierkorb')
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        return schwer(k, args.schwer[1])

    if args.antwort or args.antwort_kopf:
        return antwort(konten_finden(EDataServer.SourceRegistry.new_sync(None))[ANTWORT_KONTO], args.antwort_kopf)

    if args.adressbuecher:
        return adressbuecher(EDataServer.SourceRegistry.new_sync(None))
    if args.kontakt_live:
        if args.kontakt_live[0] not in ('anlegen', 'aendern', 'loeschen', 'pruefen', 'refresh'):
            ap.error('--kontakt-live anlegen|aendern|loeschen|pruefen|refresh BUCH DATEI')
        return kontakt_live(EDataServer.SourceRegistry.new_sync(None), *args.kontakt_live)
    if args.kontakt_graph:
        if args.kontakt_graph[0] not in ('anlegen', 'loeschen'):
            ap.error('--kontakt-graph anlegen|loeschen DATEI')
        return kontakt_graph(konten_finden(EDataServer.SourceRegistry.new_sync(None))[KONTO_M365], *args.kontakt_graph)
    if args.kontakt is not None:
        return kontakt(EDataServer.SourceRegistry.new_sync(None), args.kontakt)

    if args.entwurf:
        if len(args.entwurf) < 2 or (len(args.entwurf) > 2 and args.entwurf[2] != 'weg'):
            ap.error('--entwurf KONTO BETREFF [weg MESSAGE-ID …]')
        adresse = args.entwurf[0].lower()
        nur_testkonten([adresse])
        return entwurf(konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse], args.entwurf[1], args.entwurf[3:])
    if args.ort:
        if len(args.ort) not in (2, 3) or args.ort[1] not in ('aktion', 'konversation') or \
                args.ort[2:] not in ([], ['anlegen'], ['zurueck'], ['aufraeumen']):
            ap.error('--ort KONTO aktion|konversation [anlegen|zurueck|aufraeumen]')
        adresse = args.ort[0].lower()
        nur_testkonten([adresse])
        return ort(konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse], *args.ort[1:])

    if args.live_flags:
        adresse = args.live_flags.lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        if not k.goa.get_mail().props.imap_supported:
            ap.error('--live-flags nur für IMAP')
        return live_flags(k)

    if args.sortierung:
        adresse = args.sortierung[0].lower()
        nur_testkonten([adresse])
        if args.sortierung[1] not in ('basis', 'neu', 'papierkorb'):
            ap.error('--sortierung KONTO basis|neu|papierkorb')
        return sortierung(konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse], args.sortierung[1])

    if args.suche_zahlen:
        adresse = args.suche_zahlen[0].lower()
        nur_testkonten([adresse])
        return suche_zahlen(konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse], args.suche_zahlen[1])

    if args.konversation_flags:
        adresse = args.konversation_flags.lower()
        nur_testkonten([adresse])
        return konversation_flags(konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse])

    if args.ungelesen:
        adresse = args.ungelesen[0].lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        return ungelesen(k, args.ungelesen[1], args.soll)
    if args.anhang_teile:
        adresse = args.anhang_teile[0].lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        return anhang_teile(k, args.anhang_teile[1])
    if args.benachrichtigung:
        adresse = args.benachrichtigung[0].lower()
        if args.benachrichtigung[1] not in ('neu', 'gelesen', 'weg'):
            ap.error('--benachrichtigung KONTO neu|gelesen|weg NAMEN|UID|DATEI')
        nur_testkonten([adresse])
        return benachrichtigung(konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse],
                                *args.benachrichtigung[1:])
    if args.flags or args.server:
        adresse = (args.flags or args.server[0]).lower()
        nur_testkonten([adresse])
        k = konten_finden(EDataServer.SourceRegistry.new_sync(None))[adresse]
        if args.server and (len(args.server) not in (2, 3) or args.server[2:] not in ([], ['INBOX'], ['Papierkorb'])):
            ap.error('--server KONTO AKTION [INBOX|Papierkorb]')
        return flags(k) if args.flags else server(k, *args.server[1:], warte=args.warte)

    os.makedirs(os.path.join(CACHE, 'data'), exist_ok=True)
    os.makedirs(os.path.join(CACHE, 'cache'), exist_ok=True)
    Camel.init(CACHE, False)
    Camel.provider_init()
    EDataServer.SourceCamel.register_types()
    registry = EDataServer.SourceRegistry.new_sync(None)
    if args.anzahl:
        return anzahl(ordner_konten(registry), *args.anzahl)
    if args.zaehler is not None:
        return zaehler(ordner_konten(registry), args.zaehler)
    if args.kombiniert is not None:
        return kombiniert(ordner_konten(registry), args.kombiniert)
    if args.ordner:
        konten = ordner_konten(registry)
        session = TestSession(user_data_dir=os.path.join(CACHE, 'data'),
                              user_cache_dir=os.path.join(CACHE, 'cache'))
        session.setup({k.account.get_uid(): k.goa for k in konten})
        session.set_online(True)
        return ordner(session, konten)
    konten = konten_finden(registry)
    nur_testkonten(list(konten))
    for k in konten.values():
        nur_testkonten([k.account.get_extension(EDataServer.SOURCE_EXTENSION_AUTHENTICATION).get_user()])

    session = TestSession(user_data_dir=os.path.join(CACHE, 'data'),
                          user_cache_dir=os.path.join(CACHE, 'cache'))
    session.setup({s.get_uid(): k.goa for k in konten.values()
                   for s in (k.account, k.transport) if s})
    session.set_online(True)
    # #411: „is not None“, damit --last 0 nicht in den Standardlauf (Versand) fällt.
    if args.last is not None:
        k = konten[LAST_KONTO]
        k.store = dienst(session, k.account, EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT,
                         Camel.ProviderType.STORE)
        ordnerbaum(k.store)
        n = last(k, args.last, args.dry_run, args.last_ordner)
        k.store.disconnect_sync(True, None)
        return n
    if args.anhang:
        for a in ANHANG_KONTEN:
            store_oeffnen(session, konten[a])
        anhang(konten, args.dry_run)
        for a in ANHANG_KONTEN:
            konten[a].store.disconnect_sync(True, None)
        return 0
    if args.suche:
        for a in SUCHE_KONTEN:
            store_oeffnen(session, konten[a])
        suche(konten, args.dry_run)
        for a in SUCHE_KONTEN:
            konten[a].store.disconnect_sync(True, None)
        return 0
    if args.konversation:
        for a in KONVERSATION_KONTEN:
            store_oeffnen(session, konten[a])
        konversation(konten, registry, args.dry_run)
        for a in KONVERSATION_KONTEN:
            konten[a].store.disconnect_sync(True, None)
        return 0
    for k in konten.values():
        store_oeffnen(session, k)

    if args.pruefen:
        return 1 if pruefen(konten) else 0
    if args.anzeige:
        anzeige(konten, args.dry_run)
        for k in konten.values():
            k.store.disconnect_sync(True, None)
        return 0
    n = senden(session, konten, args.dry_run)
    if n and not args.dry_run:
        print('  warte auf Zustellung …')
        warten(konten)
    ablegen(konten, args.dry_run)
    anzeige(konten, args.dry_run)
    anhang(konten, args.dry_run)
    konversation(konten, registry, args.dry_run)
    for k in konten.values():
        k.store.disconnect_sync(True, None)
    return 0


# #411: transienter Netzabbruch (z. B. Gmail „imaplib.IMAP4.abort: socket error: EOF“): ein
# Wiederholversuch, nur für wiederholbare Aufrufe (nicht der Versand im Standardlauf, nicht
# --server … neu). Die Ausgabe eines Versuchs wird gepuffert und erst nach seinem Ende ausgegeben,
# ein abgebrochener Versuch hinterlässt also keine halben Zeilen. Jede Wiederholung steht auf stderr
# und als Zeile in der Datei TESTDATEN_WIEDERHOLT (Umgebung, tests/abbruch.sh zählt sie).
TRANSIENT = (imaplib.IMAP4.abort, ConnectionError, TimeoutError, ssl.SSLError, urllib.error.URLError)
MODI = ('--pruefen', '--ordner', '--anzeige', '--anhang', '--anhang-teile', '--last', '--flags', '--ungelesen',
        '--zaehler', '--anzahl', '--server', '--kombiniert', '--live-flags', '--schwer', '--suche-zahlen',
        '--antwort', '--antwort-kopf', '--benachrichtigung', '--zustellung', '--ort', '--ordnertest')


def main_mit_wiederholung():
    argv = sys.argv[1:]
    if not any(a in MODI for a in argv) or (('--server' in argv or '--benachrichtigung' in argv) and 'neu' in argv):
        return main()
    echt = sys.stdout
    for versuch in (1, 2):
        sys.stdout = puffer = io.StringIO()
        try:
            return main()
        except TRANSIENT as e:
            if versuch == 2 or (isinstance(e, urllib.error.HTTPError) and e.code < 500):
                raise
            text = 'WIEDERHOLUNG tests/testdaten.py %s: %s: %s' % (' '.join(argv), type(e).__name__, e)
            print(text, file=sys.stderr, flush=True)
            if os.environ.get('TESTDATEN_WIEDERHOLT'):
                with open(os.environ['TESTDATEN_WIEDERHOLT'], 'a') as f:
                    f.write(text.replace('\n', ' ') + '\n')
            puffer = None
            time.sleep(5)
        finally:
            sys.stdout = echt
            if puffer is not None:
                echt.write(puffer.getvalue())
                echt.flush()


if __name__ == '__main__':
    sys.exit(main_mit_wiederholung())
