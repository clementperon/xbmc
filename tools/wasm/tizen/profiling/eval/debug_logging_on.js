// Turn on debug logging with the audio (2048), video (32768) and A/V timing
// (262144) components in guisettings.xml. Kodi reads the file at start: close
// the page and relaunch with inspect.sh for it to take effect.
(async () => {
  const path = '/home/web_user/.kodi/userdata/guisettings.xml';
  let s = FS.readFile(path, { encoding: 'utf8' });
  const set = (id, value) => {
    const re = new RegExp('<setting id="' + id.replace('.', '\\.') + '"[^\\n]*');
    if (!re.test(s)) throw new Error('missing ' + id);
    s = s.replace(re, `<setting id="${id}">${value}</setting>`);
  };
  set('debug.showloginfo', 'true');
  set('debug.extralogging', 'true');
  set('debug.setextraloglevel', '2048,32768,262144');
  FS.writeFile(path, s);
  await new Promise((resolve, reject) => FS.syncfs(false, (err) => (err ? reject(err) : resolve())));
  return s.match(/<setting id="debug\.[a-z]+"[^\n]*/g);
})()
