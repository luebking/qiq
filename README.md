# qiq
The last shell you'll ever want

## Because your desktop is stupid.
A rant.

![image](https://guidebookgallery.org/pics/gui/desktop/full/win95.png)

Around 30 years ago and almost a year too late Microsoft introduced a new desktop shell with Windows 95 (heavily inspired by CDE where they dropped out, but that's one reason why windows, OS/2 and CDE all look the same. I digress…)

The basis is a wallpaper (more literally back then since fullscreen backgrounds required resources and older systems could only run a single color or more often a tiled small pattern)  
Anyway, it's where we would go on to put pictures of ~young females of re…~ landscapes and art - then we would litter the wallpaper with icons.

The other central element was the taskbar, it had buttons for all windows and one that said start and you infamously had to click that to stop the session.  
The taskbar also had a systray which would allow 3rd party developer to dump random stuff into the taskbar, which they quickly would abuse and the systray got at least as littered as the wallpaper.

I'm telling you that despite there's a good chance you're overly familiar with this - because it's what your desktop looks like today. At least at the baseline.  

### The concept had various problems, the main one being the desktop.
Because while littered with icons and it being where your trashcan resides, it's frequently covered up by all sorts of windows - what makes it inaccessible and useless.  
To combat that, you'd be able to show the desktop (often implemented by minimizing all windows, what comes with its own set of problems)  
**Using the by definition always lowest window (desktop) as user interface is a mistake**

### The other one was the start menu
The more applications you installed the more crowded that thing got and you'd spend a while, sliding through the submenus until you found the icon you needed to click to start the browser.  
This resulted in a list of favorites which by Murphy's rule always held the wrong couple of icons.  

It's more than 20 years ago that I drafted a replacement for KDE after noticing that it was easier to play a specific song I wanted to here in JuK than starting the browser… And I certainly wasn't the only one.  
There was quicksilver on OSX and you got things like dmenu and later rofi on X11, I'll come back to that, but
**The mouse operated start menu from a time where you had maybe 10 programs on Win3.11 is a mistake**

### The next one was the taskbar.
The window buttons were a nice way to change the active window without having to cycle through the tabbox. On occasion.  
And there's a clock you can look for the time. On occasion…  
And of course the start button you use to start programs… I guess you already know when…  
And the systray.

Very few time slices of your workday are spent with the taskbar, yet it constantly occupies screen estate.  
People came up with making it hide in various ways so it could accidentally slide into your window at the wrong moment or they would move it to the left screen edge though it's not really designed for vertical layouts.  
It also becomes a problem with fullscreen windows because you don't want to see the start button when playing a movie or game but now also your clock and all those helpful tiny systray indicators were out of reach.  
**The taskbar is a mistake**

### Oh, yeah: the systray.
The idea was certainly nice - everyone could plug their little tool into the main UI of the OS, so everyone and microsoft started to add tiny icons there - the inevitable result was that you needed filters to
configure what of that pixel mess you actually wanted to see.  
**The taskbar is a mistake**

## Changes
Despite being the predominant design for 30 years, the above is just silly and so people started to augment it with better alternatives which you probably know several of.  
Simple runners where you could type to find and start your program lead to that feature being a staple in modern start menus and instead of the tiny systray indicators,
you could place big beautiful indicators onto yor desktop where they would go on to fight with the icons who gets to to occlude most of your wallpaper. And of course usually being covered by windows…  
**Status indicators on your desktop are a mistake**  
Also the taskbar buttons aren't really required to change the window since Apple introduced Exposé

## What's left?
* Runners like rofi are generally a good idea and easily outperform any kind of start menu.  
* Notifications as one remnant of the systray are a good idea and essential part of a desktop shell (see below)
* Status indicators are a good idea (and we count the clock as being one for the time status) - but not in a tiny systray and also not on the desktop.
* And still this is improvable…

## What is a desktop shell? What is a shell?
Fundamentally, a shell, any shell, facilitates a dialog between the user and the system.  
The user requests action and information from the system and the system responds or pushes important information to the user.

## So what IS qiq? What can qiq??
The forementioned. But first, I need to address what qiq is **not**

## Qiq is NOT a terminal emulator!
A terminal emulator is not the thing where you run commands, that's a (typically POSIX) shell.  
A terminal emulator is an arcane (see what I did there?) display server, older than even X11.  
Many useful programs have been written for that display server, using ncurses or even just readline.  
Qiq cannot run *any* of those (except it can start them in a terminal emulator) and you'll continue to need one.  
**Pitfall** when you try to rm a file and typo, your shell might ask you whether you actually meant to delete all files in your $HOME directory.
Qiq will just run rm and trying to explicitly run "rm -i" will just fail because rm cannot use the terminal to ask you whether you meant to do that.

## Qiq is NOT a POSIX shell!
A POSIX shell is actually an interactive script interpreter that's just being degenerated to a command launcher because that's a very frequent task.  
Qiq supports some of the syntax features of common POSIX shells (bash or zsh) and if or when functionality is added will continue to paraphrase those.  
But that is circumstancial and while qiq can pipe|link processes, it currently cannot even redirect in- or output.  
**Shell warriors will still want and need an interactive shell, just maybe less often**

## So 80 lines in, will you now finally tell me what's in the box??
As if you hadn't skipped to here anyway.  
Qiq unites the ideas behind rofi, conky, dunst and then some.  
Because that's the essence of a desktop shell.

The primary display allows you to show random status indicators, like conky, but not behind other windows where you cannot see it.
The idea is to push a shortcut, see what you wanted to know and then push the shortcut again to make it go away.

The moment you start to type, the interface changes into a filter for your desktop applications - you're now in rofi territory.  
Select an application, hit enter, it starts and qiq goes.

However, you can also enter commands for random executables in your $PATH and if you hit the tab key, you'll even get it autocompleted.    
Rofi actually still covers that - but here's where thigs change.

There're hundred and thousands of useful console commands to query information, popular ones and custom scripts that only exist on your disk.  
You might want to query some settings a programs exposes via dbus or really just see a manpage (the UI of `man` is actually `less`) or ask `dict`
for a word you totally know but just to be… sure.  
These work flawlessly in a terminal emulator because a terminal emulator can print text, but they typically don't in these runners.  
So you'll find yourself opening a (new) terminal (tab) just to see whether some host is up or how exactly *acidamannophyn* is spelled (not like that).  
**Why?**

Qiq will wait for 3 seconds and see whether the process has finished and printed something you might want to see and then show it to you.  
You can control this and ?ask explicitly !hint that you're not interested in any response or #request a list as answer (useful for find).  
And since qiq supports aliases, you can also implement those as defaults (for commands you know are slow to respond but still interesting)  
Otherwise qiq will detach from the process like if you had run a desktop application.

## Ok… what else?
Qiq can also filter random file contents like dmenu (and  rofi, but not as a compatible drop-in, I'm afraid. At least for now) and of course invoke a calculator
(since we do have output)  
But more importantly, once you went zsh, you're never going back and that was never an option and therefore doesn't only filter and autocomplete executables,
but it also helps you to navigate through the filesystem when looking for a file you wanted to pass to that command you're entering and, if correctly configured
and desired, anytime hitting tab after entering a command will query zsh'ells autocompletion system and offer you the options.  
Qiq is a desktop shell and not your interactive text shell, but it can be as convenient.

## Hmmm… so you talked a lot about input and there's also some output, but…
Qiq comes with a notification daemon implementing https://xdg.pages.freedesktop.org/xdg-specs/notification/ (plus extra) and can also act as notification client,
so you can run `qiq notify SNAFU timeout=5s` in a script.  
This is first because these things are useful and second because qiq can then manage your notifications and leverage the thing for some magic.

**Firstly** you have some status indicators, because you care about that information, right? And sometimes you might care extra special, because the value
reached a critical level.  
Well, you can define that and since qiq is monitoring the device anyway, it will send you a notification when crossing thresholds.

**Secondly** there's an important. Really important! thing that's essentially part of every desktop, but usually stuck on your monitor or so.  
Qiq has a notebook where you can just take short notes for stuff you need to remember this week… or so.  
But here it's also a simple calendar, so if your note starts with something that looks like a date, qiq will notify you at that date about that note that you
otherwise would have forgottom and remembered one day too late.

> There shall, in that time, be rumors of things going astray, erm, and there shall be a great confusion as to where things really are, and nobody will really
> know where lieth those little things wi-- with the sort of raffia work base that has an attachment. At this time, a friend shall lose his friend's hammer and
> the young shall not know where lieth the things possessed by their fathers that their fathers put there only just the night before, about eight o'clock.