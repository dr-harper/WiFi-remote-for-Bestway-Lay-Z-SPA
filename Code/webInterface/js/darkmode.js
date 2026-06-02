// Dark-mode helper. The toggle itself only lives on /webconfig.html now
// (relocated from the header to give other UI more room), so this script
// has to be null-safe on every other page where the toggle ID is absent.
//
// Behaviour:
//   - At script-load (synchronous): re-apply the stored preference to
//     <html class="darkmode">. Runs before paint to avoid a light→dark
//     flash on dark-mode users navigating between pages.
//   - On DOMContentLoaded: if the toggle exists on this page, sync its
//     checked state to the stored preference.
//   - On toggle click: persist + apply.

(function applyStoredTheme() {
  if (localStorage.getItem('darkModeStatus') === 'On') {
    document.body.classList.add('darkmode');
  }
})();

function toggleDarkMode() {
  const toggle = document.getElementById('darkModeToggle');
  if (!toggle) return;
  if (toggle.checked) {
    document.body.classList.add('darkmode');
    localStorage.setItem('darkModeStatus', 'On');
  } else {
    document.body.classList.remove('darkmode');
    localStorage.setItem('darkModeStatus', 'Off');
  }
}

document.addEventListener('DOMContentLoaded', function () {
  const toggle = document.getElementById('darkModeToggle');
  if (toggle) {
    toggle.checked = localStorage.getItem('darkModeStatus') === 'On';
  }
});
