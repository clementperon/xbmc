// Restore the default debug logging settings changed by debug_logging_on.js.
// Takes effect at the next Kodi start.
(async () => {
  const path = '/home/web_user/.kodi/userdata/guisettings.xml';
  let s = FS.readFile(path, { encoding: 'utf8' });
  const set = (id, line) => {
    const re = new RegExp('<setting id="' + id.replace('.', '\\.') + '"[^\\n]*');
    if (!re.test(s)) throw new Error('missing ' + id);
    s = s.replace(re, line);
  };
  set('debug.showloginfo', '<setting id="debug.showloginfo" default="true">false</setting>');
  set('debug.extralogging', '<setting id="debug.extralogging" default="true">false</setting>');
  set('debug.setextraloglevel', '<setting id="debug.setextraloglevel" default="true" />');
  FS.writeFile(path, s);
  await new Promise((resolve, reject) => FS.syncfs(false, (err) => (err ? reject(err) : resolve())));
  return s.match(/<setting id="debug\.[a-z]+"[^\n]*/g);
})()
