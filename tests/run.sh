#!/bin/sh
# The checks of the test build (Linux): music files, internet radio from a
# local ICY server, favourites, the list. Needs python3 and ffmpeg (for the
# test music). Screenshots go to build/test/*.png.
set -e
cd "$(dirname "$0")/.."
T=build/test
BIN=build/picoamp_test
rm -rf $T
mkdir -p $T/Music/Album $T/cfg/Radio
fail=0
check() {   # check <name> <output> <grep pattern>
    if printf '%s\n' "$2" | grep -q -- "$3"; then echo "ok   $1"; else echo "FAIL $1: wanted '$3'"; printf '%s\n' "$2" | sed 's/^/     /'; fail=1; fi
}

E='0.25*sin(2*PI*(110+55*floor(mod(t,4)))*t)*exp(-3*mod(t,0.5))+0.15*sin(2*PI*440*t)*(0.5+0.5*sin(2*PI*0.5*t))+0.08*sin(2*PI*2200*t)*lt(mod(t,0.25),0.05)+0.04*sin(2*PI*6000*t)'
ffmpeg -loglevel error -y -f lavfi -i "aevalsrc='$E|$E*0.8':s=44100:d=30" -c:a libmp3lame -b:a 192k \
    -metadata title="Sine Café Song" -metadata artist="Unit Tester" -id3v2_version 3 "$T/Music/01 Sine.mp3"
ffmpeg -loglevel error -y -f lavfi -i "aevalsrc='$E':s=22050:d=12" -ac 1 -c:a libmp3lame -b:a 64k \
    -metadata title="A very long title that is much longer than its place on the screen" "$T/Music/02 Long.mp3"
ffmpeg -loglevel error -y -f lavfi -i "aevalsrc='$E|$E':s=48000:d=4" "$T/Music/Album/Demo.wav"

PORT=18765
WEB=18766
python3 tests/icy_server.py $PORT "$T/Music/01 Sine.mp3" 192 &
SRV=$!
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=160x160:rate=1" -frames:v 1 -pix_fmt yuvj420p $T/logo.jpg
python3 tests/web_server.py $WEB http://localhost:$PORT/mid $T/logo.jpg > $T/web.log 2>&1 &
SRV2=$!
trap 'kill $SRV $SRV2 2>/dev/null' EXIT
export PICOAMP_RB=http://localhost:$WEB PICOAMP_IPAPI=http://localhost:$WEB/ipapi PICOAMP_ITUNES=http://localhost:$WEB/itunes
export PICOAMP_METEO=http://localhost:$WEB/meteo PICOAMP_GEO=http://localhost:$WEB/geo
export PICOAMP_YAHOO=http://localhost:$WEB PICOAMP_YFEEDS=http://localhost:$WEB
export PICOAMP_LRCLIB=http://localhost:$WEB/lrclib
sleep 0.5
printf '#EXTM3U\r\n#EXTINF:-1,Local FM\r\nhttp://localhost:%d/stream.mp3\r\n#EXTINF:-1,Old SHOUTcast\r\nhttp://localhost:%d/icy\r\n#EXTINF:-1,Via PLS\r\nhttp://localhost:%d/list.pls\r\n#EXTINF:-1,AAC\r\nhttp://localhost:%d/aac\r\n#EXTINF:-1,Live\r\nhttp://localhost:%d/mid\r\n' \
    $PORT $PORT $PORT $PORT $PORT > $T/cfg/Radio/Test.m3u

run() { PICOAMP_CONFIG=$T/cfg PICOAMP_MUSIC=$PWD/$T/Music timeout 60 $BIN "$1" 2>&1; }

# the top of the list, then the music folder
out=$(run "wait 200; shot $T/root.png 3; key Down; key Enter; status; shot $T/music.png 3")
check "top list" "$out" "shot $T/root.png: ok"
[ -f $T/cfg/Radio/Stations.m3u ] && echo "ok   first start writes Radio/Stations.m3u" || { echo "FAIL Radio/Stations.m3u"; fail=1; }

# a file: tags, format, time, next, seek
sed -i "s|^folder = .*|folder = $PWD/$T/Music|" $T/cfg/picoamp.ini
out=$(run "key Down; key Down; key Enter; wait 2200; status; shot $T/playing.png 3; key .; wait 300; status; key N; wait 1200; status")
check "mp3 tags" "$out" "title='Sine Café Song' artist='Unit Tester'"
check "mp3 format" "$out" "rate=44100 ch=2 br=192"
check "plays" "$out" "state=1 elapsed=2/30"
check "seek +10" "$out" "elapsed=1[2-3]/30"
check "next track" "$out" "pl=2/2 .*title='A very long title"
check "mono 22 kHz" "$out" "rate=22050 ch=1 br=64"

# covers (V a third time): the tag's picture, a folder's cover.jpg, none
mkdir -p $T/Covers/Folder $T/Covers/Bare
ffmpeg -loglevel error -y -f lavfi -i testsrc=size=400x400:rate=1 -frames:v 1 -pix_fmt yuvj420p $T/pic.jpg
ffmpeg -loglevel error -y -f lavfi -i "aevalsrc='$E':s=44100:d=8" -i $T/pic.jpg -map 0 -map 1 -c:a libmp3lame -b:a 128k -c:v copy \
    -id3v2_version 3 -metadata title="Tagged" -metadata:s:v comment="Cover (front)" "$T/Covers/01 Tagged.mp3"
ffmpeg -loglevel error -y -f lavfi -i "aevalsrc='$E':s=44100:d=8" -c:a libmp3lame -b:a 128k "$T/Covers/Folder/Tune.mp3"
cp $T/pic.jpg $T/Covers/Folder/cover.jpg
cp "$T/Covers/Folder/Tune.mp3" "$T/Covers/Bare/Plain.mp3"
sed -i "s|^folder = .*|folder = $PWD/$T/Covers|; s|^view = .*|view = spectrum|" $T/cfg/picoamp.ini
out=$(run "key End; key Enter; wait 600; key V; key V; wait 200; page; shot $T/cover.png 3; key Home; key Down; key Enter; key Down; key Enter; wait 600; page; key BS; key Home; key Down; key Down; key Enter; key Down; key Enter; wait 600; page; key V; page")
check "cover: the tag's picture" "$out" "^page 0 cover=ready"
check "cover: no picture: a default one" "$(printf '%s\n' "$out" | sed -n 3p)" "^page 0 cover=ready"
check "cover: a folder's cover.jpg" "$(printf '%s\n' "$out" | sed -n 4p)" "^page 0 cover=ready"
check "cover: V goes on to the spectrum" "$(printf '%s\n' "$out" | sed -n 5p)" "^page 0\$"

# the radio: ICY titles, ICY 200 OK, .pls, an AAC station
sed -i "s|^folder = .*|folder = radio:Test.m3u|" $T/cfg/picoamp.ini
out=$(run "key Down; key Enter; wait 2500; status; shot $T/radio.png 3; key Down; key Enter; wait 2500; status; key Down; key Enter; wait 2500; status; key Down; key Enter; wait 1500; status; key Down; key Enter; wait 2500; status")
check "radio plays" "$out" "state=1 .*stream=1 buf=0.*title='Song [A-Za-z]* .* artist='Artist [A-Za-z]*' .*album='Local FM'"
check "ICY 200 OK" "$out" "state=1 .*album='Old SHOUTcast'"
check ".pls list" "$out" "state=1 .*album='Via PLS'"

# the help (? in the title bar, F1, ?) and the wheel on the volume
hw=$(run "click 244 4; page; key x; page; key F1; page; key F1; page; key ?; page; key Esc; status; click 200 188 wheelup; click 230 188 wheelup; status; click 170 188 wheeldown; status; list; click 100 170 wheeldown; list; quit")
check "help: the ? in the title bar" "$hw" "^page 0 help"
check "help: F1 opens and closes it" "$(printf '%s\n' "$hw" | sed -n 2,4p | tr '\n' ' ')" "page 0 page 0 help page 0 "
check "the wheel on VOL: louder" "$hw" "vol=90"
check "the wheel on VOL: softer" "$hw" "vol=85"
check "the wheel on the list: scrolls" "$hw" " >6 Local FM"
check "AAC refused" "$out" "err='AAC streams are not supported yet'"
check "joined mid-frame" "$out" "state=1 .*stream=1 buf=0.*album='Live'"

# the equalizer screen and the full screen pictures
sed -i "s|^folder = .*|folder = $PWD/$T/Music|" $T/cfg/picoamp.ini
out=$(run "key G; key Right; key Up; key Up; key Up; key Left; key Left; key Up; wait 50; shot $T/eq.png 3; key G; key Z; wait 300; shot $T/bars.png 3; key V; key V; wait 300; shot $T/plasma.png 3; key Z")
grep -q "^eq = 0,3,0,0,0" $T/cfg/picoamp.ini && echo "ok   equalizer: band 250 Hz +3" || { echo "FAIL equalizer"; grep "^eq" $T/cfg/picoamp.ini; fail=1; }
grep -q "^balance = 55" $T/cfg/picoamp.ini && echo "ok   balance" || { echo "FAIL balance"; grep "^balance" $T/cfg/picoamp.ini; fail=1; }
sed -i "s|^eq = .*|eq = 0,0,0,0,0|; s|^balance = .*|balance = 50|" $T/cfg/picoamp.ini
out=$(run "key End; key Enter; wait 800; key Z; key Left; wait 300; shot $T/fullcover.png 3; key Z")
check "full screen: the cover" "$out" "shot $T/fullcover.png: ok"

# favourites: F adds, D removes
sed -i "s|^folder = .*|folder = radio:Test.m3u|" $T/cfg/picoamp.ini
out=$(run "key Home; key Down; key F; key BS; key Home; key Down; key Enter; wait 100; shot $T/favs.png 3")
grep -q "localhost:$PORT/stream.mp3" $T/cfg/Radio/Favourites.m3u && echo "ok   F adds a favourite" || { echo "FAIL favourite add"; fail=1; }
sed -i "s|^folder = .*|folder = radio:Favourites.m3u|" $T/cfg/picoamp.ini
printf '#EXTM3U\r\n#EXTINF:-1,One\r\nhttp://one/\r\n#EXTINF:-1,Two\r\nhttp://two/\r\n' >> $T/cfg/Radio/Favourites.m3u
out=$(run "key End; key CtrlUp; key CtrlUp; list")
check "favourites: move up" "$out" "6 Two = http://two/"
grep -A1 "localhost" $T/cfg/Radio/Favourites.m3u | head -1 >/dev/null
awk '/http/{print}' $T/cfg/Radio/Favourites.m3u | head -1 | grep -q "two" && echo "ok   favourites: order kept in the file" || { echo "FAIL favourites order"; cat $T/cfg/Radio/Favourites.m3u; fail=1; }
out=$(run "key Home; key Down; key D; key D; key D")
grep -q "localhost" $T/cfg/Radio/Favourites.m3u && { echo "FAIL favourite del"; fail=1; } || echo "ok   D removes it"

# a dropped .m3u lands in the Radio folder
printf '#EXTM3U\n#EXTINF:-1,Dropped\nhttp://localhost:%d/stream.mp3\n' $PORT > $T/Dropped.m3u
out=$(run "drop $PWD/$T/Dropped.m3u; wait 50")
[ -f $T/cfg/Radio/Dropped.m3u ] && echo "ok   a dropped list is added" || { echo "FAIL dropped list"; fail=1; }

# the station directory (Radio Browser): lists, a country, more stations, search
sed -i "s|^folder = .*|folder = radio:|" $T/cfg/picoamp.ini
out=$(run "key Home; key Down; key Down; key Enter; wait 400; list; key Down; key Down; key Enter; wait 2500; status; key BS; key Down; key Down; key Down; key Enter; wait 400; list; key Down; key Down; key Enter; wait 400; list; key BS; key BS; key Down; key Enter; key Down; key Down; key Down; key Down; key Down; key Down; key Down; key Down; key Down; key Enter; wait 400; key End; key Enter; wait 400; list; key /; type jazz; key Enter; wait 400; list; shot $T/search.png 3; key /; type zzz; key Enter; wait 400")
check "directory: top stations" "$out" "list /Internet radio/Top stations (6)"
check "directory: no AAC yet" "$out" "Station 4 = http"
printf '%s\n' "$out" | grep -q "An AAC one" && { echo "FAIL directory: AAC station listed"; fail=1; } || echo "ok   directory: AAC left out"
check "directory: a station plays" "$out" "state=1 .*album='Station 1'"
grep -q "click uuid-1" $T/web.log && echo "ok   directory: the play is counted" || { echo "FAIL directory click"; fail=1; }
check "directory: countries" "$out" "list /Internet radio/By country (3)"
check "directory: a country" "$out" "NPO Radio 1 = http"
check "directory: more stations" "$out" "list /Internet radio/By genre/Jazz (101)"
check "directory: search" "$out" "list /Internet radio/Search/jazz (3)"
check "directory: HLS left out" "$out" "Smooth Jazz 24 = http"

# a station's logo as its cover: asked for by its directory id, kept in Logos; a station without one
sed -i "s|^folder = .*|folder = radio:|" $T/cfg/picoamp.ini
out=$(run "key Home; key Down; key Down; key Enter; wait 400; key Down; key Down; key Enter; wait 1500; key V; key V; wait 1500; page; shot $T/logo.png 3; key V")
check "logo: the directory's station" "$out" "^page 0 cover=ready"
ls $T/cfg/Logos/*.img > /dev/null 2>&1 && echo "ok   logo: kept" || { echo "FAIL logo: not kept"; fail=1; }
n=$(grep -c "byuuid uuid-1" $T/web.log)
sed -i "s|^folder = .*|folder = radio:|" $T/cfg/picoamp.ini
out=$(run "key Home; key Down; key Down; key Enter; wait 400; key Down; key Down; key Enter; wait 1500; key V; key V; wait 300; page; key V")
check "logo: from the cache" "$out" "^page 0 cover=ready"
[ "$(grep -c "byuuid uuid-1" $T/web.log)" = "$n" ] && echo "ok   logo: not asked again" || { echo "FAIL logo: asked again"; fail=1; }
sed -i "s|^folder = .*|folder = radio:Test.m3u|" $T/cfg/picoamp.ini
out=$(run "key Down; key Enter; wait 1500; key V; key V; wait 1500; page; status")
check "logo: a station without one: a default picture" "$out" "^page 0 cover=ready"
grep -q "itunes Artist" $T/web.log && echo "ok   song: its cover asked for (iTunes)" || { echo "FAIL song: iTunes not asked"; fail=1; }
ls $T/cfg/Covers/*.img > /dev/null 2>&1 && echo "ok   song: a cover found and kept" || { echo "FAIL song: no cover kept"; ls $T/cfg/Covers; fail=1; }
ls $T/cfg/Covers/*.none > /dev/null 2>&1 && echo "ok   song: 'none' kept for a song without one" || { echo "FAIL song: none not kept"; fail=1; }
grep -q "byurl http://localhost:$PORT/stream.mp3" $T/web.log && echo "ok   logo: asked by its address" || { echo "FAIL logo: byurl"; fail=1; }
ls $T/cfg/Logos/*.none > /dev/null 2>&1 && echo "ok   logo: 'none' kept" || { echo "FAIL logo: none not kept"; fail=1; }
sed -i "s|^folder = .*|folder = radio:|" $T/cfg/picoamp.ini

# how many are kept (keepcovers 200): 215 old copies; a "none" note older than 30 days: asked again; the
# song's answer kept, the oldest go (down to 90 %)
for i in $(seq 1 215); do f=$T/cfg/Covers/old$(printf %03d $i).img; : > $f; touch -d @$((1577836800 + i * 60)) $f; done
for f in $T/cfg/Covers/*.none; do touch -d "40 days ago" "$f"; done
n=$(grep -c "itunes" $T/web.log)
sed -i "s|^folder = .*|folder = radio:Test.m3u|" $T/cfg/picoamp.ini
out=$(run "key Down; key Enter; wait 2500; status")
[ "$(grep -c "itunes" $T/web.log)" -gt "$n" ] && echo "ok   covers: a 'none' older than 30 days: asked again" || { echo "FAIL covers: an old 'none' not asked again"; fail=1; }
c=$(ls $T/cfg/Covers | wc -l)
[ "$c" -le 200 ] && [ "$c" -ge 175 ] && echo "ok   covers: at most 200 kept ($c)" || { echo "FAIL covers: $c kept"; fail=1; }
[ ! -e $T/cfg/Covers/old001.img ] && [ -e $T/cfg/Covers/old215.img ] && echo "ok   covers: the oldest went" || { echo "FAIL covers: not the oldest"; fail=1; }
rm -f $T/cfg/Covers/old*.img
sed -i "s|^folder = .*|folder = radio:|" $T/cfg/picoamp.ini

# the clock (place by IP, by name; sleep timer; alarm) and the weather
sed -i "s|^folder = .*|folder = radio:Test.m3u|" $T/cfg/picoamp.ini
out=$(run "key Down; key Enter; wait 1500; key C; wait 1200; page; shot $T/clock.png 3; key V; shot $T/analog.png 3; key O; key A; type 6:45w; key Enter; key L; type Sydney; key Enter; wait 600; key Down; key Enter; wait 800; page; key W; wait 300; page; shot $T/weather.png 3")
check "clock: the place by IP" "$out" "place='Utrecht, NL' weather='8.4 C Rain'"
check "clock: internet time" "$out" "(internet time)"
check "clock: sleep and alarm" "$out" "Sleep 15 min  Alarm 06:45 Mo-Fr"
check "clock: a place by name" "$out" "place='Sydney, Nova Scotia, CA'"
check "weather: now" "$out" "weather place='Sydney, Nova Scotia, CA' now=8.4 code=61 (Rain)"
check "weather: the days" "$out" "2026-11-08 Sun code=71 max=31 min=-62 rain=65%"
grep -q "^alarm_time = 06:45" $T/cfg/picoamp.ini && grep -q "^alarm_days = weekdays" $T/cfg/picoamp.ini && echo "ok   alarm kept in picoamp.ini" || { echo "FAIL alarm in ini"; fail=1; }

# the news: a feed by its address, the headlines, a story, its full article, the catalogue
out=$(run "key T; wait 100; key A; type http://localhost:$WEB/feeds/rss.xml; key Enter; wait 800; key End; key Enter; wait 800; page; shot $T/news.png 2; key Down; key Enter; key Enter; wait 1000; page; shot $T/article.png 2; key Esc; key F; key Right; key Enter; key Down; key Enter; wait 50; page; key T")
check "news: a feed by its address" "$out" "6 Test News = http://localhost:$WEB/feeds/rss.xml"
check "news: headlines newest first" "$out" "news headlines feed='Test News' state=2 stories=30"
check "news: entities" "$out" "Story 25 & more"
check "news: the full article" "$out" "| Paragraph 0 of the article 24."
printf '%s\n' "$out" | grep -q "Footer text\|Menu Home" && { echo "FAIL news: menus or footer in the article"; fail=1; } || echo "ok   news: no menus or footers"
check "news: from the catalogue" "$out" "7 BBC News, top stories = https://feeds.bbci.co.uk/news/rss.xml"
grep -q 'xmlUrl="http://localhost' $T/cfg/MyFeeds.opml && echo "ok   news: MyFeeds.opml" || { echo "FAIL MyFeeds.opml"; fail=1; }

# the markets: a list's prices, the order by percent, an index's members, the tape, a chart and its headlines
out=$(run "key \$; wait 1500; page; shot $T/markets.png 2; key Down; key Down; key Down; key Down; key I; wait 1500; page; key PgDn; page; key BS; page; key O; page; key O; key O")
check "markets: Europe's indices" "$out" "markets table region=Europe list='Indices' rows=20 state=2"
check "markets: AEX's price" "$out" "\^AEX AEX [0-9]"
check "markets: the tape" "$out" "tape: \^AEX,\^GDAXI,\^FTSE,\^FCHI story 0 & more +++"
check "markets: risers first" "$out" "list='Indices' rows=20 state=2 sort=1 sel=4"
check "markets: FTSE 100's members" "$out" "list='FTSE 100' rows=98 state=2"
check "markets: the next page" "$out" "rows=98 state=2 sort=0 sel=40 top=40"
check "markets: back to the list" "$out" "list='Indices' rows=20 state=2 sort=0 sel=4"
out=$(run "key \$; wait 1500; key Enter; wait 1500; page; shot $T/markets_chart.png 2; key Right; key Right; key Right; key Right; key Right; wait 1500; page; shot $T/markets_year.png 2; key T; wait 100; page; key Esc; page; key Left; key Left; key Left; key Left; key Left")
check "markets: the day's chart" "$out" "sym=\^AEX range=1D chart=2 points=33"
check "markets: its headlines" "$out" "head: \^AEX story 5 & more"
check "markets: a year" "$out" "range=1Y chart=2 points=255"
check "markets: the headlines in the news reader" "$out" "news headlines feed='Market news: AEX' state=2 stories=6"
check "markets: and back" "$out" "markets chart .* sym=\^AEX range=1Y"
grep -q "^region = europe" $T/cfg/picoamp.ini && grep -q "^range = 1d" $T/cfg/picoamp.ini && echo "ok   markets: picoamp.ini" || { echo "FAIL markets: picoamp.ini"; fail=1; }

# the pictures (I): the browser (folders and pictures only), one picture, the slideshow, the settings
mkdir -p $T/Pictures/Holiday
ffmpeg -loglevel error -y -f lavfi -i testsrc=size=1600x1200:rate=1 -frames:v 1 -pix_fmt yuvj420p $T/Pictures/a.jpg
ffmpeg -loglevel error -y -f lavfi -i smptebars=size=640x480:rate=1 -frames:v 1 -pix_fmt yuvj420p $T/Pictures/b.jpg
ffmpeg -loglevel error -y -f lavfi -i testsrc=size=300x900:rate=1 -frames:v 1 -pix_fmt yuvj420p $T/Pictures/c.jpg
cp $T/pic.jpg $T/Pictures/Holiday/d.jpg
echo text > $T/Pictures/notes.txt
export PICOAMP_PICTURES=$PWD/$T/Pictures
sed -i "s|^interval = .*|interval = 5|" $T/cfg/picoamp.ini
out=$(run "key I; wait 100; page; shot $T/pictures.png 3; key Down; key Enter; wait 1200; page; shot $T/picture.png 3; key Right; wait 1200; page; key x; key Space; key A; page; key A; key Home; key Enter; wait 200; page; key BS; key G; wait 1200; page; wait 5800; page; key x; key O; key O; key U; key I; page")
check "pictures: folders and pictures only, sorted" "$out" "pictures browse folder='$PWD/$T/Pictures' entries=4 folders=1 pictures=3"
check "pictures: one shows (SCREEN 8)" "$out" "pictures view .*shown=1 have=1"
check "pictures: Right: the next one" "$out" "pictures view .*shown=2 have=1"
check "pictures: A marks all" "$out" "pictures=3 marked=3"
check "pictures: a folder" "$out" "pictures browse folder='$PWD/$T/Pictures/Holiday' entries=1 folders=0 pictures=1"
check "pictures: the slideshow, then the next after its time" "$out" "pictures slideshow .*shown=1 have=1"
check "pictures: the slideshow goes on" "$out" "pictures slideshow .*shown=[23] have=1"
grep -q "^saver = 2" $T/cfg/picoamp.ini && grep -q "^pictures_shuffle = yes" $T/cfg/picoamp.ini && echo "ok   pictures: the screensaver's minutes and shuffle kept" || { echo "FAIL pictures settings"; grep "saver\|shuffle" $T/cfg/picoamp.ini; fail=1; }
# the screensaver: after a minute without a key (saver = 1), any key back
sed -i "s|^saver = .*|saver = 1|" $T/cfg/picoamp.ini
out=$(timeout 100 env PICOAMP_CONFIG=$T/cfg PICOAMP_MUSIC=$PWD/$T/Music $BIN "wait 58000; page; wait 3000; page; shot $T/saver.png 3; key x; page; quit" 2>&1)
check "screensaver: not yet" "$(printf '%s\n' "$out" | sed -n 1p)" "^page 0"
check "screensaver: after a minute" "$out" "pictures screensaver .*have=1"
check "screensaver: a key, back to the player" "$(printf '%s\n' "$out" | tail -1)" "^page 0"
sed -i "s|^saver = .*|saver = 0|" $T/cfg/picoamp.ini

# another program's music (the pretend player of the test build): its track, its picture, its keys,
# its volume, shuffle and repeat; then a file here: the player's own again
sed -i "s|^folder = .*|folder = |" $T/cfg/picoamp.ini
top=$(PICOAMP_FAKE_MEDIA="Spotify|x|y|z|1" PICOAMP_CONFIG=$T/cfg PICOAMP_MUSIC=$PWD/$T/Music timeout 60 $BIN "list" 2>&1)
check "follow: its entry at the top of the list" "$top" "8 Spotify & other players"
sed -i "s|^folder = .*|folder = $PWD/$T/Music|" $T/cfg/picoamp.ini
sed -i "s|^view = .*|view = cover|" $T/cfg/picoamp.ini
vol_before=$(sed -n 's/^volume = //p' $T/cfg/picoamp.ini)
export PICOAMP_FAKE_MEDIA="Spotify|Fake Song|Fake Artist|Fake Album|200" PICOAMP_FAKE_ART=$PWD/$T/logo.jpg
out=$(run "follow; wait 400; status; shot $T/follow.png 3; page; key Space; wait 300; status; key Space; key N; wait 300; status; key .; wait 300; status; key +; key H; key R; wait 300; status; key Down; key Down; key Enter; wait 1500; status")
unset PICOAMP_FAKE_MEDIA PICOAMP_FAKE_ART
check "follow: its track" "$out" "state=1 .*title='Fake Song' artist='Fake Artist' album='Fake Album' .*system=1 source='Spotify'"
check "follow: its picture" "$out" "cover=ready"
check "follow: Space pauses it" "$out" "state=2 .*title='Fake Song' .*system=1"
check "follow: N its next track" "$out" "state=1 .*title='Fake Song 2' .*system=1"
check "follow: > seeks in it" "$out" "elapsed=1[01]/200 .*title='Fake Song 2'"
check "follow: its volume, shuffle and repeat" "$out" "vol=55 .*system=1 source='Spotify' mode=3"
check "follow: a file here ends it" "$out" "title='Sine Café Song' .*system=0"
check "follow: and pauses the program" "$out" "pretend player: paused"
[ "$(sed -n 's/^volume = //p' $T/cfg/picoamp.ini)" = "$vol_before" ] && echo "ok   follow: the player's own volume kept" || { echo "FAIL follow: the player's volume changed"; fail=1; }

# the lyrics (L, from the stand-in for LRCLIB): a file's, synced (its one song, by its length), the line
# sung now, moved with [ ]; kept (not asked again); another program's (the search: the version of its
# length, synced), then an instrumental one
sed -i "s|^folder = .*|folder = $PWD/$T/Music|" $T/cfg/picoamp.ini
out=$(run "key Down; key Down; key Enter; wait 300; key L; wait 2400; shot $T/lyrics.png 3; page; key [; key [; key [; key [; page; key 0; key Esc; page")
check "lyrics: a file's, synced, the line sung now" "$out" "lyrics state=synced lines=6 at=1 line='Second line, a rather long one"
check "lyrics: [ takes them 2 s back" "$out" "lyrics state=synced lines=6 at=0 line='First line'"
check "lyrics: ESC back" "$(printf '%s\n' "$out" | tail -1)" "^page 0"
asked=$(grep -c "lrclib get Unit Tester" $T/web.log)
out=$(run "key Down; key Down; key Enter; wait 300; key L; wait 200; page")
check "lyrics: kept" "$out" "lyrics state=synced lines=6"
[ "$(grep -c "lrclib get Unit Tester" $T/web.log)" = "$asked" ] && echo "ok   lyrics: not asked again" || { echo "FAIL lyrics: asked again"; fail=1; }
export PICOAMP_FAKE_MEDIA="Spotify|Fake Song|Fake Artist|Fake Album|200"
out=$(run "follow; wait 300; key L; wait 500; shot $T/lyrics2.png 3; page; key N; wait 700; page")
unset PICOAMP_FAKE_MEDIA
check "lyrics: another program's, the right version, synced" "$out" "lyrics state=synced lines=3 at=0 line='Fake one é'"
check "lyrics: instrumental" "$out" "lyrics state=instrumental"

# the MSX link (K): the stream (MP3 frames in time, the song in its ICY titles), the keys back, what
# plays, the cover (80 x 80 JPEG), the station list; another program's music, then a file's
sed -i "s|^msx_port = .*|msx_port = 18777|" $T/cfg/picoamp.ini
python3 tests/msx_client.py 18777 7 next prev toggle 'shuffle&on=1' > $T/msx.log 2>&1 &
CLI=$!
export PICOAMP_FAKE_MEDIA="Spotify|Fake Song|Fake Artist|Fake Album|200" PICOAMP_FAKE_ART=$PWD/$T/logo.jpg
out=$(run "msxlink on; follow; wait 9000; status")
unset PICOAMP_FAKE_MEDIA PICOAMP_FAKE_ART
wait $CLI
msx=$(cat $T/msx.log)
check "msx link: the stream" "$msx" "header content-type: audio/mpeg"
check "msx link: it says it is PicoAmp's" "$msx" "header x-picoamp: 1"
check "msx link: the song in its title" "$msx" "meta: StreamTitle='Fake Artist - Fake Song';"
check "msx link: N on the MSX: the next song here" "$msx" "meta: StreamTitle='Fake Artist - Fake Song 2';"
check "msx link: MP3 frames, one after the other" "$msx" "frames in a row: [1-9][0-9][0-9], kbps/rate: \[(160, 44100)\]"
check "msx link: pause on the MSX pauses it here" "$out" "state=2 .*title='Fake Song' .*system=1"
check "msx link: what plays" "$msx" "now: 200 {\"state\":\"paused\",\"title\":\"Fake Song\""
check "msx link: the cover, 80 x 80" "$msx" "cover size: 80x80"
check "msx link: the full screen's cover, fits 281 x 192" "$msx" "big cover: 200 [0-9]* bytes, 192x192"
check "msx link: shuffle on the MSX: shuffle here" "$msx" '"shuffle":true'
check "msx link: the station list" "$msx" "m3u: 200 #EXTM3U .*:18777/stream.mp3"
python3 tests/msx_client.py 18777 4 > $T/msx2.log 2>&1 &
CLI=$!
out=$(run "key Down; key Down; key Enter; wait 6000; msxlink off")
wait $CLI
check "msx link: a file's song" "$(cat $T/msx2.log)" "meta: StreamTitle='Unit Tester - Sine Café Song';"
grep -q "^msx_link = off" $T/cfg/picoamp.ini && echo "ok   msx link: off, kept" || { echo "FAIL msx link: not off"; fail=1; }
# its screen (the MSX as the PC's screen and remote: no sound): the song (in the MSX's characters), its
# time and state, the bars 25 times a second; seeking to a time and pause from the MSX
python3 tests/msx_client.py 18777 5.5 --screen 'seek&to=10' toggle 'vol&v=40' 'mute&on=1' > $T/msx3.log 2>&1 &
CLI=$!
out=$(run "msxlink on; key Down; key Down; key Enter; wait 7000; status; msxlink off")
wait $CLI
scr=$(cat $T/msx3.log)
check "msx screen: X-PicoAmp: 2" "$scr" "screen status: HTTP/1.0 200 OK | x-picoamp: 2"
check "msx screen: the song, in the MSX's characters" "$scr" "screen song: \['Unit Tester', 'Sine Caf.x82 Song', ''\]"
check "msx screen: its time and state" "$scr" "screen time: [0-9]* 30 p"
check "msx screen: the bars, 25 a second" "$scr" "screen bars: 1[0-9][0-9] lines in 6 s, [1-9][0-9]* with bars lit"
check "msx screen: the volume and mute from the MSX" "$out" "vol=40 .*muted=1"
check "msx screen: the volume, muted, shown there" "$scr" "screen time: [0-9]* 30 a 40 1"
check "msx screen: the words, synced, in rows of 40" "$scr" "screen words: s, 7 rows: 500 First line | 2000 Second line, a rather long one that does | 2000 not fit on one row of the page at all | 4000 Caf.x82 line, twice | 6000  | 8000 Last line | 12000 Caf.x82 line, twice"
check "msx screen: seeking to a time from the MSX" "$scr" "screen time: 1[01][0-9][0-9][0-9] 30"
check "msx screen: pause from the MSX" "$out" "state=2 .*title='Sine Café Song'"

[ $fail = 0 ] && echo "all checks passed" || { echo "some checks FAILED"; exit 1; }
