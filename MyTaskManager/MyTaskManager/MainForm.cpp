#include "MainForm.h"
#include <Windows.h>

using namespace MyTaskManager;
using namespace System;
using namespace System::Windows::Forms;

[STAThreadAttribute]
int main(array<String^>^ args) {
    Application::EnableVisualStyles();
    Application::SetCompatibleTextRenderingDefault(false);

    // Запуск головного вікна
    Application::Run(gcnew MainForm());
    return 0;
}