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
python3 tests/web_server.py $WEB http://localhost:$PORT/mid > $T/web.log 2>&1 &
SRV2=$!
trap 'kill $SRV $SRV2 2>/dev/null' EXIT
export PICOAMP_RB=http://localhost:$WEB PICOAMP_IPAPI=http://localhost:$WEB/ipapi
export PICOAMP_METEO=http://localhost:$WEB/meteo PICOAMP_GEO=http://localhost:$WEB/geo
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

# the radio: ICY titles, ICY 200 OK, .pls, an AAC station
sed -i "s|^folder = .*|folder = radio:Test.m3u|" $T/cfg/picoamp.ini
out=$(run "key Down; key Enter; wait 2500; status; shot $T/radio.png 3; key Down; key Enter; wait 2500; status; key Down; key Enter; wait 2500; status; key Down; key Enter; wait 1500; status; key Down; key Enter; wait 2500; status")
check "radio plays" "$out" "state=1 .*stream=1 buf=0.*title='Artist [A-Za-z]* - Song .*album='Local FM'"
check "ICY 200 OK" "$out" "state=1 .*album='Old SHOUTcast'"
check ".pls list" "$out" "state=1 .*album='Via PLS'"
check "AAC refused" "$out" "err='AAC streams are not supported yet'"
check "joined mid-frame" "$out" "state=1 .*stream=1 buf=0.*album='Live'"

# the equalizer screen and the full screen pictures
sed -i "s|^folder = .*|folder = $PWD/$T/Music|" $T/cfg/picoamp.ini
out=$(run "key G; key Right; key Up; key Up; key Up; key Left; key Left; key Up; wait 50; shot $T/eq.png 3; key G; key Z; wait 300; shot $T/bars.png 3; key V; key V; wait 300; shot $T/plasma.png 3; key Z")
grep -q "^eq = 0,3,0,0,0" $T/cfg/picoamp.ini && echo "ok   equalizer: band 250 Hz +3" || { echo "FAIL equalizer"; grep "^eq" $T/cfg/picoamp.ini; fail=1; }
grep -q "^balance = 55" $T/cfg/picoamp.ini && echo "ok   balance" || { echo "FAIL balance"; grep "^balance" $T/cfg/picoamp.ini; fail=1; }
sed -i "s|^eq = .*|eq = 0,0,0,0,0|; s|^balance = .*|balance = 50|" $T/cfg/picoamp.ini

# favourites: F adds, D removes
sed -i "s|^folder = .*|folder = radio:Test.m3u|" $T/cfg/picoamp.ini
out=$(run "key Home; key Down; key F; key BS; key Home; key Down; key Enter; wait 100; shot $T/favs.png 3")
grep -q "localhost:$PORT/stream.mp3" $T/cfg/Radio/Favourites.m3u && echo "ok   F adds a favourite" || { echo "FAIL favourite add"; fail=1; }
sed -i "s|^folder = .*|folder = radio:Favourites.m3u|" $T/cfg/picoamp.ini
out=$(run "key Home; key Down; key D")
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

[ $fail = 0 ] && echo "all checks passed" || { echo "some checks FAILED"; exit 1; }
